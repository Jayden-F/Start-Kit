#include "CompetitionSystem.h"
#include "nlohmann/json.hpp"
#include <Logger.h>
#include <boost/tokenizer.hpp>
#include <functional>
#include <unistd.h>

using json = nlohmann::ordered_json;

bool BaseSystem::all_tasks_complete() {
  for (auto &t : assigned_tasks) {
    if (!t.empty()) {
      return false;
    }
  }
  return true;
}

void BaseSystem::task_book_keeping(vector<Action> &actions) {

  list<Task> finished_tasks_this_timestep; // <agent_id, task_id, timestep>
                                           //
  for (int k = 0; k < num_of_agents; k++) {
    if (!assigned_tasks[k].empty() &&
        curr_states[k].location == assigned_tasks[k].front().location) {
      Task task = assigned_tasks[k].front();
      assigned_tasks[k].pop_front();
      task.t_completed = timestep;
      finished_tasks_this_timestep.push_back(task);
      events[k].push_back(make_tuple(task.task_id, timestep, "finished"));
      // log_event_finished(k, task.task_id, timestep);
    }
    paths[k].push_back(curr_states[k]);
    // Use this paths[] to verify for 1-robustness in MAPF-DP, generate a
    // warning for now? Feels like I should add a MAPF-DP flag?

    actual_movements[k].push_back(actions[k]);
  }
  // update tasks
  //
  for (auto task : finished_tasks_this_timestep) {
    finished_tasks[task.agent_assigned].emplace_back(task);
    num_of_task_finish++;
  }

  // TODO: this should be dependency injection
  update_tasks();
}

void BaseSystem::move(vector<Action> &actions) {
  // actions.resize(num_of_agents, Action::NA);
  // ------------------------------
  for (int k = 0; k < num_of_agents; k++) {
    // log->log_plan(false,k);
    if (k >= actions.size()) {
      fast_mover_feasible = false;
      planner_movements[k].push_back(Action::NA);
    } else {
      planner_movements[k].push_back(actions[k]);
    }
  }

  if (!valid_moves(curr_states, actions)) {
    fast_mover_feasible = false;
    actions = std::vector<Action>(num_of_agents, Action::W);
  }

  // ------------------------------

  // apply real actions
  // ------------------------------
  vector<State> next_states = model->result_states(curr_states, actions);
  std::cout << "Next states: ";
  for (int i = 0; i < next_states.size(); i++) {
    std::cout << std::to_string(next_states.at(i).location) << " ";
  }
  std::cout << std::endl;

  curr_states = next_states;
  //if (simulate_each_step) {
  //executor->send_plan(curr_states, next_states);
  //curr_states = executor->get_agent_locations(timestep);
  //} else {
  //curr_states = next_states;
  //}
}

// This function might not work correctly with small map (w or h <=2)
bool BaseSystem::valid_moves(vector<State> &prev, vector<Action> &action) {
  return model->is_valid(prev, action);
}

void BaseSystem::sync_shared_env() {

  if (!started) {
    env->goal_locations.resize(num_of_agents);
    for (size_t i = 0; i < num_of_agents; i++) {
      env->goal_locations[i].clear();
      for (auto &task : assigned_tasks[i]) {
        env->goal_locations[i].push_back({task.location, task.t_assigned});
      }
    }
    env->curr_states = curr_states;
  }
  env->curr_timestep = timestep;
}

vector<Action> BaseSystem::plan_wrapper() {
  vector<Action> actions;
  planner->plan(plan_time_limit, actions);

  return actions;
}

vector<Action> BaseSystem::plan() {
  using namespace std::placeholders;
  if (started &&
      future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    std::cout << started << "     "
              << (future.wait_for(std::chrono::seconds(0)) !=
                  std::future_status::ready)
              << std::endl;
    if (logger) {
      logger->log_info(
          "planner cannot run because the previous run is still running",
          timestep);
    }

    if (future.wait_for(std::chrono::seconds(plan_time_limit)) ==
        std::future_status::ready) {
      task_td.join();
      started = false;
      return future.get();
    }
    logger->log_info("planner timeout", timestep);
    return {};
  }

  std::packaged_task<std::vector<Action>()> task(
      std::bind(&BaseSystem::plan_wrapper, this));
  future = task.get_future();
  if (task_td.joinable()) {
    task_td.join();
  }
  task_td = std::thread(std::move(task));
  started = true;
  if (future.wait_for(std::chrono::seconds(plan_time_limit)) ==
      std::future_status::ready) {
    task_td.join();
    started = false;
    return future.get();
  }
  logger->log_info("planner timeout", timestep);
  return {};
}

bool BaseSystem::planner_initialize() {
  using namespace std::placeholders;
  std::packaged_task<void(int)> init_task(
      std::bind(&MAPFPlanner::initialize, planner, _1));
  auto init_future = init_task.get_future();

  auto init_td = std::thread(std::move(init_task), preprocess_time_limit);
  if (init_future.wait_for(std::chrono::seconds(preprocess_time_limit)) ==
      std::future_status::ready) {
    init_td.join();
    return true;
  }

  init_td.detach();
  return false;
}

void BaseSystem::log_preprocessing(bool succ) {
  if (logger == nullptr)
    return;
  if (succ) {
    logger->log_info("Preprocessing success", timestep);
  } else {
    logger->log_fatal("Preprocessing timeout", timestep);
  }
  logger->flush();
}

void BaseSystem::log_event_assigned(int agent_id, int task_id, int timestep) {
  logger->log_info("Task " + std::to_string(task_id) +
                       " is assigned to agent " + std::to_string(agent_id),
                   timestep);
}

void BaseSystem::log_event_finished(int agent_id, int task_id, int timestep) {
  logger->log_info("Agent " + std::to_string(agent_id) + " finishes task " +
                       std::to_string(task_id),
                   timestep);
}

// find a plan
// TODO:
// get state from sim
//  -- status and configuration
//  UPDATE
// task assignment
// execution policy
//  -- could call planner or use buffered actions
//  SEND ACTIONS
//  validate actions
//  simulate action
//
void BaseSystem::simulate(int simulation_time) {
  // init logger
  // Logger* log = new Logger();
  initialize();

  // Start by getting agent locations from central controller
  // TODO: This looks weird, I only need this when I want dynamic start.
  //curr_states = executor->get_agent_locations(timestep);

  for (; timestep < simulation_time;) {

    sync_shared_env();

    auto start = std::chrono::steady_clock::now();
    vector<Action> actions = plan();
    auto end = std::chrono::steady_clock::now();

    if (!planner_movements[0].empty() &&
        planner_movements[0].back() == Action::NA) {
      planner_times.back() +=
          plan_time_limit; // add planning time to last record
    } else {
      auto diff = end - start;
      planner_times.push_back(std::chrono::duration<double>(diff).count());
    }

    // Increment agent plan costs
    timestep += 1;
    for (int a = 0; a < num_of_agents; a++) {
      if (!env->goal_locations[a].empty())
        solution_costs[a]++;
    }

    // validate, simulate and book keeping completed tasks
    move(actions);
    task_book_keeping(actions);

    if (all_tasks_complete()) {
      break;
    }
  }
}

void BaseSystem::initialize() {
  paths.resize(num_of_agents);
  events.resize(num_of_agents);
  env->num_of_agents = num_of_agents;
  env->rows = map.rows;
  env->cols = map.cols;
  env->map = map.map;
  finished_tasks.resize(num_of_agents);
  // bool succ = load_records(); // continue simulating from the records
  timestep = 0;
  curr_states = starts;
  assigned_tasks.resize(num_of_agents);

  // planner initialise before knowing the first goals
  bool planner_initialize_success = planner_initialize();

  log_preprocessing(planner_initialize_success);
  if (!planner_initialize_success)
    _exit(124);

  // initialize_goal_locations();
  update_tasks();

  sync_shared_env();

  actual_movements.resize(num_of_agents);
  planner_movements.resize(num_of_agents);
  solution_costs.resize(num_of_agents);
  for (int a = 0; a < num_of_agents; a++) {
    solution_costs[a] = 0;
  }
}

void BaseSystem::savePaths(const string &fileName, int option) const {
  std::ofstream output;
  output.open(fileName, std::ios::out);
  for (int i = 0; i < num_of_agents; i++) {
    output << "Agent " << i << ": ";
    if (option == 0) {
      bool first = true;
      for (const auto t : actual_movements[i]) {
        if (!first) {
          output << ",";
        } else {
          first = false;
        }
        output << t;
      }
    } else if (option == 1) {
      bool first = true;
      for (const auto t : planner_movements[i]) {
        if (!first) {
          output << ",";
        } else {
          first = false;
        }
        output << t;
      }
    } else if (option == 3) {
      bool first = true;
      for (const auto t : paths[i]) {
        if (!first) {
          output << ",";
        } else {
          first = false;
        }
        output << t;
      }
    }
    output << endl;
  }
  output.close();
}

void BaseSystem::saveResults(const string &fileName, int screen) const {
  json js;
  // Save action model
  js["actionModel"] = "MAPF_T";

  std::string feasible = fast_mover_feasible ? "Yes" : "No";
  js["AllValid"] = feasible;

  js["teamSize"] = num_of_agents;

  // Save start locations[x,y,orientation]
  if (screen <= 2) {
    json start = json::array();
    for (int i = 0; i < num_of_agents; i++) {
      json s = json::array();
      s.push_back(starts[i].location / map.cols);
      s.push_back(starts[i].location % map.cols);
      switch (starts[i].orientation) {
      case 0:
        s.push_back("E");
        break;
      case 1:
        s.push_back("S");
      case 2:
        s.push_back("W");
        break;
      case 3:
        s.push_back("N");
        break;
      }
      start.push_back(s);
    }
    js["start"] = start;
  }

  js["numTaskFinished"] = num_of_task_finish;
  int sum_of_cost = 0;
  int makespan = 0;
  if (num_of_agents > 0) {
    sum_of_cost = solution_costs[0];
    makespan = solution_costs[0];
    for (int a = 1; a < num_of_agents; a++) {
      sum_of_cost += solution_costs[a];
      if (solution_costs[a] > makespan) {
        makespan = solution_costs[a];
      }
    }
  }
  js["sumOfCost"] = sum_of_cost;
  js["makespan"] = makespan;

  if (screen <= 2) {
    // Save actual paths
    json apaths = json::array();
    for (int i = 0; i < num_of_agents; i++) {
      std::string path;
      bool first = true;
      for (const auto action : actual_movements[i]) {
        if (!first) {
          path += ",";
        } else {
          first = false;
        }

        if (action == Action::FW) {
          path += "F";
        } else if (action == Action::CR) {
          path += "R";
        } else if (action == Action::CCR) {
          path += "C";
        } else if (action == Action::NA) {
          path += "T";
        } else {
          path += "W";
        }
      }
      apaths.push_back(path);
    }
    js["actualPaths"] = apaths;
  }

  if (screen <= 1) {
    // planned paths
    json ppaths = json::array();
    for (int i = 0; i < num_of_agents; i++) {
      std::string path;
      bool first = true;
      for (const auto action : planner_movements[i]) {
        if (!first) {
          path += ",";
        } else {
          first = false;
        }

        if (action == Action::FW) {
          path += "F";
        } else if (action == Action::CR) {
          path += "R";
        } else if (action == Action::CCR) {
          path += "C";
        } else if (action == Action::NA) {
          path += "T";
        } else {
          path += "W";
        }
      }
      ppaths.push_back(path);
    }
    js["plannerPaths"] = ppaths;

    json planning_times = json::array();
    for (double time : planner_times)
      planning_times.push_back(time);
    js["plannerTimes"] = planning_times;

    // Save errors
    json errors = json::array();
    for (auto error : model->errors) {
      std::string error_msg;
      int agent1;
      int agent2;
      int timestep;
      std::tie(error_msg, agent1, agent2, timestep) = error;
      json e = json::array();
      e.push_back(agent1);
      e.push_back(agent2);
      e.push_back(timestep);
      e.push_back(error_msg);
      errors.push_back(e);
    }
    js["errors"] = errors;

    // Save events
    json events_json = json::array();
    for (int i = 0; i < num_of_agents; i++) {
      json event = json::array();
      for (auto e : events[i]) {
        json ev = json::array();
        std::string event_msg;
        int task_id;
        int timestep;
        std::tie(task_id, timestep, event_msg) = e;
        ev.push_back(task_id);
        ev.push_back(timestep);
        ev.push_back(event_msg);
        event.push_back(ev);
      }
      events_json.push_back(event);
    }
    js["events"] = events_json;

    // Save all tasks
    json tasks = json::array();
    for (auto t : all_tasks) {
      json task = json::array();
      task.push_back(t.task_id);
      task.push_back(t.location / map.cols);
      task.push_back(t.location % map.cols);
      tasks.push_back(task);
    }
    js["tasks"] = tasks;
  }

  std::ofstream f(fileName, std::ios_base::trunc | std::ios_base::out);
  f << std::setw(4) << js;
}

bool FixedAssignSystem::load_agent_tasks(string fname) {
  string line;
  std::ifstream myfile(fname.c_str());
  if (!myfile.is_open())
    return false;

  getline(myfile, line);
  while (!myfile.eof() && line[0] == '#') {
    getline(myfile, line);
  }

  boost::char_separator<char> sep(",");
  boost::tokenizer<boost::char_separator<char>> tok(line, sep);
  boost::tokenizer<boost::char_separator<char>>::iterator beg = tok.begin();

  num_of_agents = atoi((*beg).c_str());
  int task_id = 0;
  // My benchmark
  if (num_of_agents == 0) {
    // issue_logs.push_back("Load file failed");
    std::cerr << "The number of agents should be larger than 0" << endl;
    exit(-1);
  }
  starts.resize(num_of_agents);
  task_queue.resize(num_of_agents);

  for (int i = 0; i < num_of_agents; i++) {

    getline(myfile, line);
    while (!myfile.eof() && line[0] == '#')
      getline(myfile, line);

    boost::tokenizer<boost::char_separator<char>> tok(line, sep);
    boost::tokenizer<boost::char_separator<char>>::iterator beg = tok.begin();
    // read start [row,col] for agent i
    int num_landmarks = atoi((*beg).c_str());
    beg++;
    auto loc = atoi((*beg).c_str());
    // agent_start_locations[i] = {loc, 0};
    starts[i] = State(loc, 0, 0);
    beg++;
    for (int j = 0; j < num_landmarks; j++, beg++) {
      auto loc = atoi((*beg).c_str());
      task_queue[i].emplace_back(task_id++, loc, 0, i);
    }
  }
  myfile.close();

  return true;
}

void FixedAssignSystem::update_tasks() {
  for (int k = 0; k < num_of_agents; k++) {
    while (assigned_tasks[k].size() < num_tasks_reveal &&
           !task_queue[k].empty()) {
      Task task = task_queue[k].front();
      task_queue[k].pop_front();
      assigned_tasks[k].push_back(task);
      events[k].push_back(make_tuple(task.task_id, timestep, "assigned"));
      all_tasks.push_back(task);
      // log_event_assigned(k, task.task_id, timestep);
    }
  }
}

void TaskAssignSystem::update_tasks() {
  for (int k = 0; k < num_of_agents; k++) {
    while (assigned_tasks[k].size() < num_tasks_reveal && !task_queue.empty()) {
      Task task = task_queue.front();
      task.t_assigned = timestep;
      task.agent_assigned = k;
      task_queue.pop_front();
      assigned_tasks[k].push_back(task);
      events[k].push_back(make_tuple(task.task_id, timestep, "assigned"));
      all_tasks.push_back(task);
      // log_event_assigned(k, task.task_id, timestep);
    }
  }
}

void InfAssignSystem::update_tasks() {
  for (int k = 0; k < num_of_agents; k++) {
    while (assigned_tasks[k].size() < num_tasks_reveal) {
      int i = task_counter[k] * num_of_agents + k;
      int loc = tasks[i % tasks_size];
      Task task(task_id, loc, timestep, k);
      assigned_tasks[k].push_back(task);
      events[k].push_back(make_tuple(task.task_id, timestep, "assigned"));
      // log_event_assigned(k, task.task_id, timestep);
      all_tasks.push_back(task);
      task_id++;
      task_counter[k]++;
    }
  }
}
