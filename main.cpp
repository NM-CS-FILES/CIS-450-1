/*
┌───┐Arrive    ┌─────┐Dispatch    ┌───────┐Terminate    ┌──────────┐
│New├─────────►│Ready├───────────►│Running├────────────►│Terminated│
└───┘          │     │◄───────────┤       │             └──────────┘
			   └─────┘     Preempt└───┬───┘
				  ▲                   │
				  │                   │IO Request
		IO Recieve│     ┌───────┐     │
				  └─────┤Blocked│◄────┘
						└───────┘
*/

#include <iostream>
#include <vector>
#include <fstream>
#include <filesystem>
#include <string>
#include <queue>
#include <cassert>
#include <cstdarg>

using namespace std;
using namespace std::filesystem;

//
//

enum job_state {
	JOB_NEW,
	JOB_READY,
	JOB_RUNNING,
	JOB_BLOCKED,
	JOB_TERMINATED
};

struct job {
	job_state state;

	uint64_t jid;

	uint64_t arrival_time;

	uint64_t turn_around_time;
	uint64_t wait_time;
	uint64_t io_time;

	queue<uint64_t> bursts;
};

template<>
struct less<job> {
	bool operator()(const job& a, const job& b) {
		return a.arrival_time > b.arrival_time;
	}
};

//
//

enum event_type {
	EVT_TERMINATE,
	EVT_ARRIVE,	
	EVT_IO_RECIEVE,
	EVT_IO_REQUEST,
	EVT_PREEMPT,   
};

struct event {
	event_type type;
	uint64_t jid;
	uint64_t arrival_time;
};

template<>
struct less<event> {
	bool operator()(const event& a, const event& b) {
		if (a.arrival_time == b.arrival_time) {
			return a.type > b.type;
		}

		return a.arrival_time > b.arrival_time;
	}
};

//
//

deque<uint64_t> READY_Q;
deque<uint64_t> IO_Q;

priority_queue<event> EVENT_Q;

vector<job> JOBS;
uint64_t TIME;
uint64_t QUANTUM;
uint64_t CPU_IDLE_TIME;

job* CPU_JOB;
job* IO_JOB;

ofstream LOG_FOUT;
ofstream OUT_FOUT;

//
//


string format(const char* fmt, va_list vargs) {
	string ret;
	size_t ret_size = vsnprintf(nullptr, 0, fmt, vargs);
	ret.resize(ret_size);
	vsnprintf(ret.data(), ret_size + 1, fmt, vargs);
	return ret;
}

string format(const char* fmt, ...) {
	va_list vargs;
	va_start(vargs, fmt);
	string ret = format(fmt, vargs);
	va_end(vargs);
	return ret;
}

void outf(const char* fmt, ...) {
	string fmted;

	va_list vargs;
	va_start(vargs, fmt);
	fmted = format(fmt, vargs);
	va_end(vargs);

	cout << TIME << ": " << fmted << endl;
	OUT_FOUT << TIME << ": " << fmted << endl;
	LOG_FOUT << TIME << ": " << fmted << endl;
}

void logf(const char* fmt, ...) {
	string fmted;

	va_list vargs;
	va_start(vargs, fmt);
	fmted = format(fmt, vargs);
	va_end(vargs);

	LOG_FOUT << TIME << ": " << fmted << endl;
}

void logf_fatal(const char* fmt, ...) {
	string fmted;

	va_list vargs;
	va_start(vargs, fmt);
	fmted = format(fmt, vargs);
	va_end(vargs);

	LOG_FOUT << TIME << ": [FATAL]: " << fmted << endl;

	exit(-1);
}

#define logf_assert(_cond_, _fmt_, ...)	if (!(_cond_))	logf_fatal((_fmt_), __VA_ARGS__)

//
//

job& get_job(uint64_t jid) {
	logf_assert(jid < JOBS.size(), "Job with id = %llu could not be found", jid);
	return JOBS.at(jid);
}

bool is_cpu_idle() {
	return CPU_JOB == nullptr;
}

bool is_io_idle() {
	return IO_JOB == nullptr;
}

job& job_from_jid(uint64_t pid) {
	return JOBS.at(pid);
}

//
//

void arrived(job& j) {
	logf_assert(j.state == JOB_NEW, "Job must be new to arrive");
	outf("P%llu Arrives - Enters Ready Queue", j.jid);

	j.state = JOB_READY;
	READY_Q.push_back(j.jid);
}

void preempted() {
	logf_assert(!is_cpu_idle(), "CPU cannot be idle while preempted");
	logf_assert(CPU_JOB->state == JOB_RUNNING, "CPU active job must be running to preempt");
	
	outf("P%llu Preempted - Moved to Ready Queue", CPU_JOB->jid);

	CPU_JOB->state = JOB_READY;
	CPU_JOB->bursts.front() -= QUANTUM;
	READY_Q.push_back(CPU_JOB->jid);
	CPU_JOB = nullptr;
}

void terminated() {
	logf_assert(!is_cpu_idle(), "CPU cannot be idle while terminating");
	logf_assert(CPU_JOB->state == JOB_RUNNING, "CPU active job must be running to terminate");

	outf("P%llu Terminated", CPU_JOB->jid);

	CPU_JOB->state = JOB_TERMINATED;
	CPU_JOB->turn_around_time = TIME - CPU_JOB->arrival_time;
	CPU_JOB = nullptr;
}

void io_requested() {
	logf_assert(!is_cpu_idle(), "CPU cannot be idle while requesting io");
	logf_assert(CPU_JOB->state == JOB_RUNNING, "CPU active job must be running to perform io request");
	
	outf("P%llu IO Blocked", CPU_JOB->jid);

	CPU_JOB->state = JOB_BLOCKED;
	CPU_JOB->bursts.pop();
	IO_Q.push_back(CPU_JOB->jid);
	CPU_JOB = nullptr;
}

void io_recieved() {
	logf_assert(!is_io_idle(), "IO cannot be idle while recieving io");
	logf_assert(IO_JOB->state == JOB_BLOCKED, "IO active job must be blocked to recieve io");

	outf("P%llu IO Done", IO_JOB->jid);

	IO_JOB->state = JOB_READY;
	IO_JOB->bursts.pop();

	READY_Q.push_back(IO_JOB->jid);

	IO_JOB = nullptr;
}

//
//

void dispatch() {
	if (READY_Q.empty()) {
		return;
	}

	CPU_JOB = &get_job(READY_Q.front());
	READY_Q.pop_front();
	
	CPU_JOB->state = JOB_RUNNING;
	
	logf_assert(!CPU_JOB->bursts.empty(), "Cannot dispatch a job with no bursts left");
	outf("P%llu Dispatched To CPU, Burst = %llu", CPU_JOB->jid, CPU_JOB->bursts.front());

	if (CPU_JOB->bursts.front() > QUANTUM) {
		EVENT_Q.push({ EVT_PREEMPT, CPU_JOB->jid, TIME + QUANTUM });
	}
	else {
		EVENT_Q.push({
			CPU_JOB->bursts.size() == 1 ? EVT_TERMINATE : EVT_IO_REQUEST,
			CPU_JOB->jid,
			TIME + CPU_JOB->bursts.front()
		});
	}

}

void io_dispatch() {
	if (IO_Q.empty()) {
		return;
	}

	IO_JOB = &get_job(IO_Q.front());
	IO_Q.pop_front();

	logf_assert(!IO_JOB->bursts.empty(), "Cannot dispatch an io job with no bursts left");
	outf("P%llu Dispatched To IO, Burst = %llu", IO_JOB->jid, IO_JOB->bursts.front());

	EVENT_Q.push({ EVT_IO_RECIEVE, IO_JOB->jid, TIME + IO_JOB->bursts.front() });

}

//
//

bool jobs_parse_line(
	const string& line, 
	job& j
) {
	stringstream line_in(line);

	if (!(line_in >> j.arrival_time)) {
		return false;
	}

	if (j.arrival_time < 1) {
		return false;
	}
	
	uint64_t cpu_burst_count = 0;

	if (!(line_in >> cpu_burst_count)) {
		return false;
	}

	if (cpu_burst_count < 1) {
		return false;
	}

	uint64_t burst_duration = 0;

	while (line_in >> burst_duration) {
		j.bursts.push(burst_duration);
	}

	if (j.bursts.size() != cpu_burst_count * 2 - 1) {
		return false;
	}

	j.state = JOB_NEW;
	j.turn_around_time = 0;
	j.wait_time = 0;
	j.io_time = 0;

	return true;
}

bool jobs_parse_file(
	const path& f_path
) {
	logf_assert(exists(f_path), "Input file must exist to parse jobs");

	ifstream f_in(f_path);
	logf_assert(f_in.is_open(), "Input file must be readable");

	string f_line;
	job j;

	while (getline(f_in, f_line)) {
		j = { };
		
		if (jobs_parse_line(f_line, j)) {
			j.jid = JOBS.size();

			JOBS.push_back(j);
			EVENT_Q.push({ EVT_ARRIVE, j.jid, j.arrival_time});
		}
		else {
			outf("Error Parsing Line... Ignoring, '%s'", f_line.c_str());
		}
	}

	return f_in.eof();
}

//
//

void sim_init(
	const char* f_out_path,
	const char* f_in_path,
	const char* f_log_path,
	const char* cpu_quantum
) {
	TIME = 0;

	if (!exists(f_in_path) || is_directory(f_in_path)) {
		printf("Invalid Input File Path\n");
		exit(-1);
	}

	char* pend = nullptr;
	QUANTUM = strtoull(cpu_quantum, &pend, 10);

	if (pend == nullptr || *pend != '\0') {
		printf("Invalid CPU Quantum Provided\n");
		exit(-1);
	}

	LOG_FOUT.open(f_log_path);
	OUT_FOUT.open(f_out_path);

	if (!LOG_FOUT.is_open()) {
		printf("Unable To Open Log File\n");
		exit(-1);
	}

	if (!OUT_FOUT.is_open()) {
		printf("Unable To Open Output File\n");
		exit(-1);
	}

	assert(jobs_parse_file(f_in_path));

	outf("Sim Started With Time Quantum of %llu\n", QUANTUM);
}

void sim_output_state() {
	stringstream stss("Sim State:\n");

	if (!is_cpu_idle() && !CPU_JOB->bursts.empty()) {
		stss << "CPU = P" << CPU_JOB->jid << ", Burst = " << CPU_JOB->bursts.front() << endl;
	}
	else {
		stss << "CPU = NULL\n";
	}

	if (!is_io_idle() && !IO_JOB->bursts.empty()) {
		stss << "IO  = P" << IO_JOB->jid << ", Burst = " << IO_JOB->bursts.front() << endl;
	}
	else {
		stss << "IO  = null\n";
	}

	stss << "Ready Q: { ";
	for (size_t i = 0; i != READY_Q.size(); i++) {
		if (i != 0) {
			stss << ", ";
		}
		stss << "P" << READY_Q.at(i);
	}
	stss << " }\n";

	stss << "IO    Q: { ";
	for (size_t i = 0; i != IO_Q.size(); i++) {
		if (i != 0) {
			stss << ", ";
		}
		stss << "P" << IO_Q.at(i);
	}
	stss << " }\n";

	string sts = stss.str();

	OUT_FOUT << sts;
	LOG_FOUT << sts;
	cout << sts;
}

void sim_tally() {
	for (uint64_t jid : READY_Q) {
		get_job(jid).wait_time++;
	}

	for (uint64_t jid : IO_Q) {
		get_job(jid).io_time++;
	}

	if (is_cpu_idle()) {
		CPU_IDLE_TIME++;
	}
}

void sim_inc_time() {
	sim_tally();

	if ((++TIME) % 5 == 0) {
		sim_output_state();
	}
}

bool sim_advance() {
	if (EVENT_Q.empty()) {
		return false;
	}

	while (TIME < EVENT_Q.top().arrival_time) {
		logf("No Event");
		sim_inc_time();
	}

	while (!EVENT_Q.empty() && TIME == EVENT_Q.top().arrival_time) {
		event evt = EVENT_Q.top();
		EVENT_Q.pop();

		switch (evt.type) {
		case EVT_ARRIVE:     arrived(get_job(evt.jid)); break;
		case EVT_PREEMPT:    preempted();    break;
		case EVT_TERMINATE:  terminated();   break;
		case EVT_IO_REQUEST: io_requested(); break;
		case EVT_IO_RECIEVE: io_recieved();  break;
		}
	}


	if (is_cpu_idle()) {
		dispatch();
	}

	if (is_io_idle()) {
		io_dispatch();
	}

	sim_inc_time();

	return true;
}

//
//

int main(int argc, char** argv) {
	
	if (argc != 5) {
		printf("Invalid Number of Arguments\nUsage *.exe path/to/output path/to/input path/to/log <time quantum>\n");
		exit(-1);
	}

	sim_init(argv[1], argv[2], argv[3], argv[4]);
	
	while (sim_advance());

	double cpu_active_time = (TIME - CPU_IDLE_TIME);
	double cpu_util = (cpu_active_time / (double)TIME * 100);

	OUT_FOUT << "CPU Utilization: " << cpu_util << endl;
	cout << "CPU Utilization: " << cpu_util << endl;

	double avg_tot = 0.f;
	double avg_io = 0.f;
	double avg_ready = 0.f;
	string fmted;

	for (uint64_t i = 0; i != JOBS.size(); i++) {
		job& j = JOBS[i];

		fmted = format("P%llu (TAT = %4llu | Ready = %4llu | IO = %4llu)\n", i,
			j.turn_around_time, j.wait_time, j.io_time);

		OUT_FOUT << fmted;
		cout << fmted;

		avg_tot += j.turn_around_time;
		avg_io += j.io_time;
		avg_ready += j.wait_time;
	}
	
	avg_tot /= (double)JOBS.size();
	avg_io /= (double)JOBS.size();
	avg_ready /= (double)JOBS.size();

	fmted = format("Average, TOT = %lf, IO = %lf, READY = %lf\n", avg_tot, avg_io, avg_ready);

	OUT_FOUT << fmted;
	cout << fmted;
}