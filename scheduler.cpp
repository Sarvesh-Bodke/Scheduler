#include <iostream>
#include <fstream>
#include <deque>
#include <vector>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <algorithm>

using namespace std;

typedef enum {
    CREATED,
    READY,
    RUNNING,
    BLOCKED
} process_state_t;

typedef enum {
    TRANS_TO_READY,
    TRANS_TO_RUN,
    TRANS_TO_BLOCK,
    TRANS_TO_PREEMPT
} transition_t;

int* randvals = nullptr;
int randval_count = 0;
int ofs = 0;

int myrandom(int burst) {
    if (burst <= 0) return 0;
    int v = 1 + (randvals[ofs % randval_count] % burst);
    ofs++;
    return v;
}

class Process {
public:
    int pid;
    int AT, TC, CB, IO;
    int static_prio;
    int dynamic_prio;

    process_state_t state;
    int state_ts;

    int FT;
    int IT; 
    int CW; 

    int remaining_time;
    int current_cpu_burst;

    Process(int pid_, int at, int tc, int cb, int io, int prio) {
        pid = pid_;
        AT = at; TC = tc; CB = cb; IO = io;
        static_prio = prio;
        dynamic_prio = prio - 1;
        state = CREATED;
        state_ts = at;

        FT = 0; IT = 0; CW = 0;
        remaining_time = tc;
        current_cpu_burst = 0;
    }
};

class Event {
public:
    int timestamp;
    Process* proc;
    transition_t transition;
    long long id; 

    Event(int ts, Process* p, transition_t tr, long long id_) {
        timestamp = ts; proc = p; transition = tr; id = id_;
    }
};

class EventQueue {
private:
    deque<Event*> events;

public:
    void put_event(Event* evt) {
        if (events.empty() || events.back()->timestamp < evt->timestamp ||
            (events.back()->timestamp == evt->timestamp && events.back()->id <= evt->id)) {
            events.push_back(evt);
            return;
        }
        for (auto it = events.begin(); it != events.end(); ++it) {
            if ((*it)->timestamp > evt->timestamp ||
               ((*it)->timestamp == evt->timestamp && (*it)->id > evt->id)) {
                events.insert(it, evt);
                return;
            }
        }
        events.push_back(evt);
    }

    Event* get_event() {
        if (events.empty()) return nullptr;
        Event* e = events.front();
        events.pop_front();
        return e;
    }

    int get_next_event_time() {
        if (events.empty()) return -1;
        return events.front()->timestamp;
    }
    
    bool empty() { return events.empty(); }
    
    void rm_event(Process* proc) {
        for (auto it = events.begin(); it != events.end(); ++it) {
            if ((*it)->proc == proc) {
                delete *it;
                events.erase(it);
                return;
            }
        }
    }

    int get_next_event_time_for(Process* proc) {
        for (Event* e : events) {
            if (e->proc == proc) return e->timestamp;
        }
        return -1;
    }

    string dump_queue() {
        string s;
        for (auto e : events) {
            char buf[128];
            snprintf(buf, sizeof(buf), "(t=%d,p=%d,tr=%d) ", e->timestamp, e->proc->pid, (int)e->transition);
            s += buf;
        }
        return s;
    }

    ~EventQueue() {
        for (auto e : events) delete e;
        events.clear();
    }
};

class Scheduler {
public:
    virtual void add_process(Process* p) = 0;
    virtual Process* get_next_process() = 0;
    virtual int get_quantum() { return 1000000000; }
    virtual bool test_preempt(Process* newly_ready, Process* curr_running) { return false; }
    virtual const char* get_name() = 0;
    virtual ~Scheduler() {}
};

class FCFS : public Scheduler {
    deque<Process*> q;
public:
    void add_process(Process* p) override { q.push_back(p); }
    Process* get_next_process() override {
        if (q.empty()) return nullptr;
        Process* p = q.front(); q.pop_front(); return p;
    }
    const char* get_name() override { return "FCFS"; }
};

class LCFS : public Scheduler {
    deque<Process*> q;
public:
    void add_process(Process* p) override { q.push_back(p); }
    Process* get_next_process() override {
        if (q.empty()) return nullptr;
        Process* p = q.back(); q.pop_back(); return p;
    }
    const char* get_name() override { return "LCFS"; }
};

class SRTF : public Scheduler {
    deque<Process*> q;
public:
    void add_process(Process* p) override { q.push_back(p); }
    Process* get_next_process() override {
        if (q.empty()) return nullptr;
        int best = 0;
        for (int i = 1; i < (int)q.size(); ++i) {
            if (q[i]->remaining_time < q[best]->remaining_time) best = i;
        }
        Process* p = q[best];
        q.erase(q.begin() + best);
        return p;
    }
    const char* get_name() override { return "SRTF"; }
};

class RR : public Scheduler {
    deque<Process*> q;
    int quantum;
public:
    RR(int q_) : quantum(q_) {}
    void add_process(Process* p) override { q.push_back(p); }
    Process* get_next_process() override {
        if (q.empty()) return nullptr;
        Process* p = q.front(); q.pop_front(); return p;
    }
    int get_quantum() override { return quantum; }
    const char* get_name() override {
        static char name[64];
        snprintf(name, sizeof(name), "RR %d", quantum);
        return name;
    }
};

class PRIO : public Scheduler {
protected:
    int quantum;
    int maxprio;
    vector<deque<Process*>> activeQ;
    vector<deque<Process*>> expiredQ;
public:
    PRIO(int q_, int m_) : quantum(q_), maxprio(m_), activeQ(m_), expiredQ(m_) {}
    virtual ~PRIO() {}

    virtual void add_process(Process* p) override {
        if (p->dynamic_prio < 0) {
            p->dynamic_prio = p->static_prio - 1;
            expiredQ[p->dynamic_prio].push_back(p);
        } else {
            activeQ[p->dynamic_prio].push_back(p);
        }
    }

    virtual Process* get_next_process() override {
        for (int pr = maxprio-1; pr >= 0; --pr) {
            if (!activeQ[pr].empty()) {
                Process* p = activeQ[pr].front();
                activeQ[pr].pop_front();
                return p;
            }
        }
        activeQ.swap(expiredQ);
        for (int pr = maxprio-1; pr >= 0; --pr) {
            if (!activeQ[pr].empty()) {
                Process* p = activeQ[pr].front();
                activeQ[pr].pop_front();
                return p;
            }
        }
        return nullptr;
    }
    int get_quantum() override { return quantum; }
    const char* get_name() override {
        static char buf[64];
        snprintf(buf, sizeof(buf), "PRIO %d", quantum);
        return buf;
    }
};

class PREPRIO : public PRIO {
public:
    PREPRIO(int q_, int m_) : PRIO(q_, m_) {}
    bool test_preempt(Process* newly_ready, Process* curr_running) override {
        if (!curr_running) return false;
        return newly_ready->dynamic_prio > curr_running->dynamic_prio;
    }
    const char* get_name() override {
        static char buf[64];
        snprintf(buf, sizeof(buf), "PREPRIO %d", quantum);
        return buf;
    }
};

EventQueue eventQ;
Scheduler* THE_SCHED = nullptr;
Process* CURRENT_RUNNING = nullptr;
vector<Process*> processes;
vector<pair<int,int>> io_periods;
long long global_event_id = 0;

int CURRENT_TIME = 0;
int cpu_busy_time = 0;
bool opt_v = false; 
bool opt_t = false; 
bool opt_e = false; 
bool opt_p = false; 

const char* state_name(process_state_t s) {
    switch(s) {
        case CREATED: return "CREATED";
        case READY: return "READY";
        case RUNNING: return "RUNNG";
        case BLOCKED: return "BLOCK";
        default: return "UNKNOWN";
    }
}

void Simulation() {
    bool call_scheduler = false;

    while (!eventQ.empty()) {
        Event* ev = eventQ.get_event();
        if (!ev) break;
        Process* proc = ev->proc;
        CURRENT_TIME = ev->timestamp;
        int timeInPrevState = CURRENT_TIME - proc->state_ts;
        transition_t trans = ev->transition;
        
        if (opt_v || opt_t) {
            printf("%d %d %2d: ", CURRENT_TIME, proc->pid, timeInPrevState);
        }
        
        delete ev;

        switch (trans) {
            case TRANS_TO_READY: {
                if (opt_v || opt_t) printf("%s -> READY\n", state_name(proc->state));
                bool from_blocked = (proc->state == BLOCKED);
                
                if (from_blocked) {
                    proc->dynamic_prio = proc->static_prio - 1;
                }
                
                // Show preemption decision for PREPRIO scheduler (when -p flag is set)
                if (opt_p && from_blocked) {
                    int cond1 = 0;
                    int cond2 = 0;
                    int will_preempt = 0;
                    
                    if (CURRENT_RUNNING) {
                        bool should_preempt = THE_SCHED->test_preempt(proc, CURRENT_RUNNING);
                        int running_future_time = eventQ.get_next_event_time_for(CURRENT_RUNNING);
                        
                        cond1 = should_preempt ? 1 : 0;
                        cond2 = (running_future_time > CURRENT_TIME) ? 1 : 0;
                        will_preempt = (cond1 && cond2) ? 1 : 0;
                    }
                    
                    printf("    --> PrioPreempt Cond1=%d Cond2=%d (%d) --> %s\n",
                           cond1, cond2, will_preempt, will_preempt ? "YES" : "NO");
                    
                    if (will_preempt) {
                        eventQ.rm_event(CURRENT_RUNNING);
                        eventQ.put_event(new Event(CURRENT_TIME, CURRENT_RUNNING, TRANS_TO_PREEMPT, global_event_id++));
                    }
                } else {
                    // Not showing -p output, but still need to check for actual preemption
                    if (THE_SCHED->test_preempt(proc, CURRENT_RUNNING) && CURRENT_RUNNING) {
                        int running_future_time = eventQ.get_next_event_time_for(CURRENT_RUNNING);
                        if (running_future_time > CURRENT_TIME) {
                            eventQ.rm_event(CURRENT_RUNNING);
                            eventQ.put_event(new Event(CURRENT_TIME, CURRENT_RUNNING, TRANS_TO_PREEMPT, global_event_id++));
                        }
                    }
                }
                
                proc->state = READY;
                proc->state_ts = CURRENT_TIME;
                THE_SCHED->add_process(proc);
                call_scheduler = true;
                break;
            }

            case TRANS_TO_RUN: {
                if (opt_v || opt_t) printf("%s -> RUNNG", state_name(proc->state));
                
                if (proc->current_cpu_burst == 0) {
                    int cb = myrandom(proc->CB);
                    if (cb > proc->remaining_time) cb = proc->remaining_time;
                    proc->current_cpu_burst = cb;
                    if (opt_v || opt_t) printf(" cb=%d rem=%d prio=%d", cb, proc->remaining_time, proc->dynamic_prio);
                } else {
                    if (opt_v || opt_t) printf(" cb=%d rem=%d prio=%d", proc->current_cpu_burst, proc->remaining_time, proc->dynamic_prio);
                }
                if (opt_v || opt_t) printf("\n");
                proc->state = RUNNING;
                proc->state_ts = CURRENT_TIME;
                proc->CW += timeInPrevState;
                int exec_time = proc->current_cpu_burst;
                transition_t next_trans = TRANS_TO_BLOCK;
                int quantum = THE_SCHED->get_quantum();
                if (exec_time > quantum) {
                    exec_time = quantum;
                    next_trans = TRANS_TO_PREEMPT;
                }
                eventQ.put_event(new Event(CURRENT_TIME + exec_time, proc, next_trans, global_event_id++));
                break;
            }

            case TRANS_TO_BLOCK: {
                proc->remaining_time -= timeInPrevState;
                if (proc->remaining_time < 0) proc->remaining_time = 0;
                proc->current_cpu_burst = 0;
                cpu_busy_time += timeInPrevState;
                if (proc->remaining_time == 0) {
                    if (opt_v || opt_t) printf("%s -> DONE\n", state_name(proc->state));
                    proc->FT = CURRENT_TIME;
                    CURRENT_RUNNING = nullptr;
                } else {
                    int ib = myrandom(proc->IO);
                    if (opt_v || opt_t) printf("%s -> BLOCK ib=%d rem=%d\n", state_name(proc->state), ib, proc->remaining_time);
                    proc->IT += ib;
                    io_periods.emplace_back(CURRENT_TIME, CURRENT_TIME + ib);
                    proc->state = BLOCKED;
                    proc->state_ts = CURRENT_TIME;
                    eventQ.put_event(new Event(CURRENT_TIME + ib, proc, TRANS_TO_READY, global_event_id++));
                    CURRENT_RUNNING = nullptr;
                }
                call_scheduler = true;
                break;
            }

            case TRANS_TO_PREEMPT: {
                proc->remaining_time -= timeInPrevState;
                if (proc->remaining_time < 0) proc->remaining_time = 0;
                proc->current_cpu_burst -= timeInPrevState;
                if (proc->current_cpu_burst < 0) proc->current_cpu_burst = 0;
                cpu_busy_time += timeInPrevState;
                if (opt_v || opt_t) printf("%s -> READY  cb=%d rem=%d prio=%d\n", 
                                           state_name(proc->state), 
                                           proc->current_cpu_burst, 
                                           proc->remaining_time, 
                                           proc->dynamic_prio);
                proc->dynamic_prio -= 1;
                string sname = THE_SCHED->get_name();
                if (sname.rfind("RR", 0) == 0) {
                    proc->dynamic_prio = proc->static_prio - 1;
                }
                
                proc->state = READY;
                proc->state_ts = CURRENT_TIME;
                THE_SCHED->add_process(proc);
                CURRENT_RUNNING = nullptr;
                call_scheduler = true;
                break;
            }
        } 
        
        if (opt_e && !eventQ.empty()) {
            printf("EventQ: %s\n", eventQ.dump_queue().c_str());
        }

        if (call_scheduler) {
            if (eventQ.get_next_event_time() == CURRENT_TIME) {
                continue;
            }
            call_scheduler = false;  
            if (CURRENT_RUNNING == nullptr) {
                Process* nxt = THE_SCHED->get_next_process();
                if (nxt) {
                    CURRENT_RUNNING = nxt;
                    eventQ.put_event(new Event(CURRENT_TIME, nxt, TRANS_TO_RUN, global_event_id++));
                }
            }
        }
    } 
}

void print_help(const char* progname) {
    printf("Usage: %s [-h] [-v] [-t] [-e] [-p] -s<schedspec> inputfile randfile\n", progname);
    printf("\nOptions:\n");
    printf("  -h           Show this help message and exit\n");
    printf("  -v           Verbose mode (show detailed state transitions)\n");
    printf("  -t           Trace events (show event execution details)\n");
    printf("  -e           Show event queue after each event\n");
    printf("  -p           Show preemption decisions (E scheduler)\n");
    printf("  -s<spec>     Scheduler specification (required)\n");
    printf("\nScheduler specifications:\n");
    printf("  -sF          FCFS (First Come First Served)\n");
    printf("  -sL          LCFS (Last Come First Served)\n");
    printf("  -sS          SRTF (Shortest Remaining Time First)\n");
    printf("  -sR<quantum> Round Robin with time quantum\n");
    printf("               Example: -sR10\n");
    printf("  -sP<quantum>[:<maxprio>] Priority scheduler\n");
    printf("               Example: -sP10 or -sP10:5\n");
    printf("  -sE<quantum>[:<maxprio>] Preemptive Priority scheduler\n");
    printf("               Example: -sE10 or -sE10:5\n");
    printf("\nArguments:\n");
    printf("  inputfile    Input file with process specifications\n");
    printf("  randfile     File with random numbers\n");
    printf("\nExamples:\n");
    printf("  %s -sF input1 rfile\n", progname);
    printf("  %s -sR10 input2 rfile\n", progname);
    printf("  %s -v -t -sP5:4 input3 rfile\n", progname);
}

int main(int argc, char* argv[]) {
    int opt;
    string sarg;
    while ((opt = getopt(argc, argv, "hvteps:")) != -1) {
        switch (opt) {
            case 'h':
                print_help(argv[0]);
                return 0;
            case 's':
                sarg = string(optarg);
                break;
            case 'v': opt_v = true; break;
            case 't': opt_t = true; break;
            case 'e': opt_e = true; break;
            case 'p': opt_p = true; break;
            default:
                print_help(argv[0]);
                return 1;
        }
    }

    if (sarg.empty()) {
        cout << "Error: Scheduler specification (-s) is required\n\n";
        print_help(argv[0]);
        return 1;
    }

    char c = sarg[0];
    int quantum = 1000000000; 
    int maxprio = 4;
    if (c == 'R' || c == 'P' || c == 'E') {
        string rest = sarg.substr(1);
        if (!rest.empty()) {
            size_t pos = rest.find(':');
            if (pos == string::npos) {
                quantum = stoi(rest);
            } else {
                quantum = stoi(rest.substr(0, pos));
                maxprio = stoi(rest.substr(pos + 1));
            }
        }
    }

    if (optind >= argc) { 
        cout << "Error: Missing inputfile\n\n";
        print_help(argv[0]);
        return 1;
    }
    char* inputfile = argv[optind++];
    
    if (optind >= argc) { 
        cout << "Error: Missing randfile\n\n";
        print_help(argv[0]);
        return 1;
    }
    char* randfile = argv[optind++];

    ifstream rf(randfile);
    if (!rf) { 
        cout << "Error: Cannot open randfile: " << randfile << "\n";
        return 1;
    }
    rf >> randval_count;
    randvals = new int[randval_count];
    for (int i = 0; i < randval_count; ++i) rf >> randvals[i];
    rf.close();

    if (c == 'F') THE_SCHED = new FCFS();
    else if (c == 'L') THE_SCHED = new LCFS();
    else if (c == 'S') THE_SCHED = new SRTF();
    else if (c == 'R') THE_SCHED = new RR(quantum);
    else if (c == 'P') THE_SCHED = new PRIO(quantum, maxprio);
    else if (c == 'E') THE_SCHED = new PREPRIO(quantum, maxprio);
    else {
        cout << "Error: Unknown scheduler spec: " << sarg << "\n\n";
        print_help(argv[0]);
        return 1;
    }

    ifstream inf(inputfile);
    if (!inf) { 
        cout << "Error: Cannot open inputfile: " << inputfile << "\n";
        return 1;
    }
    int at, tc, cb, io;
    int pid = 0;
    while (inf >> at >> tc >> cb >> io) {
        int prio = myrandom(maxprio); 
        Process* p = new Process(pid++, at, tc, cb, io, prio);
        processes.push_back(p);
        eventQ.put_event(new Event(at, p, TRANS_TO_READY, global_event_id++));
    }
    inf.close();

    Simulation();

    int finishing_time = 0;
    int total_turnaround = 0;
    int total_wait = 0;

    for (Process* p : processes) {
        finishing_time = max(finishing_time, p->FT);
        total_turnaround += (p->FT - p->AT);
        total_wait += p->CW;
    }

    int io_busy_time = 0;
    if (!io_periods.empty()) {
        sort(io_periods.begin(), io_periods.end());
        int cur_s = io_periods[0].first;
        int cur_e = io_periods[0].second;
        for (size_t i = 1; i < io_periods.size(); ++i) {
            if (io_periods[i].first > cur_e) {
                io_busy_time += (cur_e - cur_s);
                cur_s = io_periods[i].first;
                cur_e = io_periods[i].second;
            } else {
                cur_e = max(cur_e, io_periods[i].second);
            }
        }
        io_busy_time += (cur_e - cur_s);
    }

    cout << THE_SCHED->get_name() << "\n";
    for (Process* p : processes) {
        int TT = p->FT - p->AT;
        printf("%04d: %4d %4d %4d %4d %1d | %5d %5d %5d %5d\n",
               p->pid, p->AT, p->TC, p->CB, p->IO, p->static_prio,
               p->FT, TT, p->IT, p->CW);
    }

    double cpu_util = finishing_time > 0 ? 100.0 * (cpu_busy_time / (double)finishing_time) : 0.0;
    double io_util = finishing_time > 0 ? 100.0 * (io_busy_time / (double)finishing_time) : 0.0;
    double avg_turn = (double) total_turnaround / processes.size();
    double avg_wait = (double) total_wait / processes.size();
    double throughput = finishing_time > 0 ? 100.0 * (processes.size() / (double)finishing_time) : 0.0;

    printf("SUM: %d %.2lf %.2lf %.2lf %.2lf %.3lf\n",
           finishing_time, cpu_util, io_util, avg_turn, avg_wait, throughput);

    delete THE_SCHED;
    delete[] randvals;
    for (Process* p : processes) delete p;
    processes.clear();

    return 0;
}