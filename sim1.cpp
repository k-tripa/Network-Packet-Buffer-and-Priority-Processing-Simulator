#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#define GETCWD _getcwd
#else
#include <unistd.h>
#define GETCWD getcwd
#endif

using namespace std;

const int NUM_PRIORITIES = 4;
const double LINK_BPS = 1000000.0;                              // 1 Mbps
const double MEAN_SIZE_BYTES = (64 + 1500) / 2.0;               // sizes ~ U[64, 1500]
const double SERVICE_RATE = LINK_BPS / (MEAN_SIZE_BYTES * 8.0); // ~159.85 packets/s

//Packet 
struct Packet {
    long id = 0;          // arrival number (for display)
    double arrival = 0;   // arrival time (s)
    int priority = 1;     // 1 = highest
    int size = 0;         // bytes
    double service = 0;   // transmission time (s)
    long seq = 0;         // assigned on arrival, keeps FIFO among equal priorities
};

struct PacketOrder {  
    bool operator()(const Packet& a, const Packet& b) const {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.seq < b.seq;
    }
};

// Binary heap 
template <class T, class Less>
class BinaryHeap {
    vector<T> a;
    Less less;

    void siftUp(size_t i) {
        while (i > 0) {
            size_t p = (i - 1) / 2;
            if (less(a[i], a[p])) { swap(a[i], a[p]); i = p; }
            else break;
        }
    }
    void siftDown(size_t i) {
        size_t n = a.size();
        for (;;) {
            size_t l = 2 * i + 1, r = l + 1, best = i;
            if (l < n && less(a[l], a[best])) best = l;
            if (r < n && less(a[r], a[best])) best = r;
            if (best == i) break;
            swap(a[i], a[best]);
            i = best;
        }
    }

public:
    bool empty() const { return a.empty(); }
    size_t size() const { return a.size(); }
    const T& top() const { return a.front(); }
    void push(const T& x) { a.push_back(x); siftUp(a.size() - 1); }
    T pop() {
        T r = a.front();
        a.front() = a.back();
        a.pop_back();
        if (!a.empty()) siftDown(0);
        return r;
    }
    vector<T> sortedSnapshot() const {  // for display only
        BinaryHeap c = *this;
        vector<T> out;
        while (!c.empty()) out.push_back(c.pop());
        return out;
    }
    bool isValid() const {  // heap property check, used by tests
        for (size_t i = 1; i < a.size(); i++)
            if (less(a[i], a[(i - 1) / 2])) return false;
        return true;
    }
};

using PacketBuffer = BinaryHeap<Packet, PacketOrder>;

//Metrics 
struct Metrics {
    size_t capacity = 0;
    long arrived = 0, processed = 0, dropped = 0;
    long maxOccupancy = 0;
    double totalWait = 0, busyTime = 0, occupancyArea = 0;
    double startTime = 0, endTime = 0;
    vector<long> procByPrio, dropByPrio;
    vector<double> waitByPrio;

    double duration() const { return endTime - startTime; }
    double avgWait() const { return processed ? totalWait / processed : 0.0; }
    double dropRate() const { return arrived ? (double)dropped / arrived : 0.0; }
    double bufferUtilization() const {  // average occupancy / capacity
        return (capacity && duration() > 0) ? occupancyArea / (duration() * capacity) : 0.0;
    }
    double throughput() const { return duration() > 0 ? processed / duration() : 0.0; }
    double avgWaitPrio(size_t p) const {
        return (p < procByPrio.size() && procByPrio[p]) ? waitByPrio[p] / procByPrio[p] : 0.0;
    }
};

static string bufferString(const PacketBuffer& b) {
    ostringstream os;
    os << "[";
    bool first = true;
    for (const Packet& p : b.sortedSnapshot()) {
        os << (first ? "" : " ") << "#" << p.id << "(p" << p.priority << ")";
        first = false;
    }
    os << "]";
    return os.str();
}

// Simulator 
Metrics simulate(vector<Packet> pkts, size_t capacity, bool verbose = false,
                 vector<long>* serviceOrder = nullptr) {
    stable_sort(pkts.begin(), pkts.end(),
                [](const Packet& a, const Packet& b) { return a.arrival < b.arrival; });

    queue<Packet> arrivals;  // the FIFO queue of incoming packets
    for (const Packet& p : pkts) arrivals.push(p);

    Metrics m;
    m.capacity = capacity;
    if (pkts.empty()) return m;

    const double INF = numeric_limits<double>::infinity();
    PacketBuffer buf;  // the bounded buffer (priority queue)
    bool busy = false;
    Packet cur;
    double depart = INF, now = pkts.front().arrival, last = now;
    long seq = 0;
    m.startTime = now;

    auto grow = [&](int pr) {
        if ((size_t)pr >= m.procByPrio.size()) {
            m.procByPrio.resize(pr + 1, 0);
            m.dropByPrio.resize(pr + 1, 0);
            m.waitByPrio.resize(pr + 1, 0.0);
        }
    };
    auto startService = [&](const Packet& p) {
        busy = true;
        cur = p;
        depart = now + p.service;
        double w = now - p.arrival;
        m.totalWait += w;
        m.waitByPrio[p.priority] += w;
        m.busyTime += p.service;
        if (serviceOrder) serviceOrder->push_back(p.id);
    };

    if (verbose) cout << fixed << setprecision(4);

    while (!arrivals.empty() || busy) {
        double ta = arrivals.empty() ? INF : arrivals.front().arrival;

        if (ta <= depart) {  //arrival event
            now = ta;
            m.occupancyArea += buf.size() * (now - last);
            last = now;
            Packet p = arrivals.front();
            arrivals.pop();
            p.seq = seq++;
            m.arrived++;
            grow(p.priority);
            const char* what;
            if (!busy) {
                startService(p);
                what = "-> service";
            } else if (buf.size() < capacity) {
                buf.push(p);
                m.maxOccupancy = max<long>(m.maxOccupancy, buf.size());
                what = "-> buffered";
            } else {
                m.dropped++;
                m.dropByPrio[p.priority]++;
                what = "-> DROPPED (buffer full)";
            }
            if (verbose)
                cout << "t=" << setw(8) << now << "  ARRIVE #" << p.id << " (p" << p.priority
                     << ", " << p.size << "B) " << what << "\n           buffer "
                     << buf.size() << "/" << capacity << " " << bufferString(buf) << "\n";
        } else {  // departure event
            now = depart;
            m.occupancyArea += buf.size() * (now - last);
            last = now;
            busy = false;
            depart = INF;
            m.processed++;
            m.procByPrio[cur.priority]++;
            if (verbose) cout << "t=" << setw(8) << now << "  DEPART #" << cur.id << " done\n";
            if (!buf.empty()) {
                Packet nx = buf.pop();
                startService(nx);
                if (verbose)
                    cout << "           start #" << nx.id << " (p" << nx.priority << ", waited "
                         << (now - nx.arrival) * 1000 << " ms)\n           buffer " << buf.size()
                         << "/" << capacity << " " << bufferString(buf) << "\n";
            }
        }
    }
    m.endTime = now;
    return m;
}

// Generation 
vector<Packet> generatePackets(size_t n, double lambda, unsigned seed) {
    mt19937_64 rng(seed);
    exponential_distribution<double> gap(lambda);
    uniform_int_distribution<int> sz(64, 1500);
    discrete_distribution<int> pr({10, 20, 30, 40});

    vector<Packet> v;
    v.reserve(n);
    double t = 0;
    for (size_t i = 0; i < n; i++) {
        t += gap(rng);
        Packet p;
        p.id = (long)i;
        p.arrival = t;
        p.priority = pr(rng) + 1;
        p.size = sz(rng);
        p.service = p.size * 8.0 / LINK_BPS;
        v.push_back(p);
    }
    return v;
}

.
vector<Packet> loadFile(const string& path) {
    vector<Packet> v;
    ifstream in(path);
    if (!in) { cerr << "cannot open " << path << "\n"; return v; }
    string line;
    long id = 0, skipped = 0;
    while (getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        istringstream is(line);
        Packet p;
        if (!(is >> p.arrival >> p.priority >> p.size) || p.priority < 1 ||
            p.priority > NUM_PRIORITIES || p.size <= 0) {
            skipped++;  
            continue;
        }
        p.id = id++;
        p.service = p.size * 8.0 / LINK_BPS;
        v.push_back(p);
    }
    if (skipped) cerr << "warning: skipped " << skipped << " invalid line(s) in " << path << "\n";
    return v;
}

// Output 
void printMetrics(const Metrics& m, const vector<long>* order = nullptr) {
    cout << fixed << setprecision(4);
    if (order) {
        cout << "\nProcessing sequence (packet ids): ";
        for (long id : *order) cout << "#" << id << " ";
        cout << "\n";
    }
    cout << "\n--- Summary ---\n"
         << "Arrived:            " << m.arrived << "\n"
         << "Processed:          " << m.processed << "\n"
         << "Dropped:            " << m.dropped << " (" << 100 * m.dropRate() << "%)\n"
         << "Avg waiting time:   " << m.avgWait() * 1000 << " ms\n"
         << "Buffer utilization: " << 100 * m.bufferUtilization() << "%  (max occupancy "
         << m.maxOccupancy << "/" << m.capacity << ")\n"
         << "Throughput:         " << m.throughput() << " packets/s\n";
    for (size_t p = 1; p < m.procByPrio.size(); p++)
        cout << "  priority " << p << ": processed " << m.procByPrio[p] << ", dropped "
             << m.dropByPrio[p] << ", avg wait " << m.avgWaitPrio(p) * 1000 << " ms\n";
}

// Tests 
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name)                                         \
    do {                                                          \
        if (cond) { g_pass++; cout << "  [PASS] " << name << "\n"; } \
        else { g_fail++; cout << "  [FAIL] " << name << "\n"; }    \
    } while (0)

static Packet mk(long id, double t, int prio, double service, int size = 100) {
    Packet p;
    p.id = id; p.arrival = t; p.priority = prio; p.service = service; p.size = size;
    return p;
}

static bool approx(double a, double b, double eps = 1e-9) { return fabs(a - b) < eps; }

int runTests() {
    cout << "=== Tests ===\n";

    {  // heap
        PacketBuffer h;
        mt19937 rng(1);
        for (int i = 0; i < 1000; i++) {
            Packet p; p.priority = 1 + rng() % NUM_PRIORITIES; p.seq = i; h.push(p);
        }
        bool ok = h.isValid();
        Packet prev = h.pop();
        while (!h.empty()) {
            Packet c = h.pop();
            if (PacketOrder()(c, prev)) ok = false;
            prev = c;
        }
        CHECK(ok, "heap pops in (priority, seq) order for 1000 random items");
    }
    {  // empty input
        Metrics m = simulate({}, 5);
        CHECK(m.arrived == 0 && m.processed == 0 && m.dropped == 0, "empty arrival stream");
    }
    {  // FIFO within equal priority
        vector<Packet> v;
        v.push_back(mk(0, 0.0, 1, 10));
        for (int i = 1; i <= 5; i++) v.push_back(mk(i, 0.1 * i, 1, 1));
        vector<long> order;
        simulate(v, 10, false, &order);
        CHECK((order == vector<long>{0, 1, 2, 3, 4, 5}), "equal priorities are served FIFO");
    }
    {  // priority order
        vector<Packet> v = {mk(0, 0.0, 4, 10), mk(1, 0.1, 3, 1), mk(2, 0.2, 1, 1),
                            mk(3, 0.3, 2, 1), mk(4, 0.4, 1, 1)};
        vector<long> order;
        simulate(v, 10, false, &order);
        CHECK((order == vector<long>{0, 2, 4, 3, 1}), "higher priority first, FIFO inside a level");
    }
    {  // overflow
        vector<Packet> v;
        for (int i = 0; i < 5; i++) v.push_back(mk(i, 0.1 * i, 1, 10));
        Metrics m = simulate(v, 2);
        CHECK(m.processed == 3 && m.dropped == 2, "overflow: 1 in service + 2 buffered, 2 dropped");
        CHECK(m.maxOccupancy == 2, "occupancy never exceeds capacity");
    }
    {  // capacity 0
        vector<Packet> v = {mk(0, 0, 1, 5), mk(1, 1, 1, 5), mk(2, 2, 1, 5), mk(3, 20, 1, 5)};
        Metrics m = simulate(v, 0);
        CHECK(m.processed == 2 && m.dropped == 2, "capacity 0: only packets finding idle router survive");
    }
    {  // capacity 1
        vector<Packet> v = {mk(0, 0, 1, 5), mk(1, 1, 1, 5), mk(2, 2, 1, 5)};
        Metrics m = simulate(v, 1);
        CHECK(m.processed == 2 && m.dropped == 1, "capacity 1: one waits, next is dropped");
    }
    {  // simultaneous arrivals
        vector<Packet> v = {mk(0, 0, 2, 1), mk(1, 0, 2, 1), mk(2, 0, 2, 1), mk(3, 0, 2, 1)};
        vector<long> order;
        Metrics m = simulate(v, 10, false, &order);
        CHECK((order == vector<long>{0, 1, 2, 3}) && m.dropped == 0,
              "simultaneous equal-priority arrivals keep input order");
    }
    {  // tie: arrival at the exact departure instant is handled first
        vector<Packet> v = {mk(0, 0, 1, 1), mk(1, 1, 1, 1)};
        Metrics m = simulate(v, 0);
        CHECK(m.dropped == 1, "arrival-before-departure tie rule");
    }
    {  // waiting time arithmetic
        vector<Packet> v = {mk(0, 0, 1, 2), mk(1, 1, 1, 1)};
        Metrics m = simulate(v, 5);
        CHECK(approx(m.avgWait(), 0.5) && approx(m.endTime, 3.0),
              "avg wait = (0 + 1) / 2 and end time = 3");
    }
    {  // buffer utilization arithmetic
        vector<Packet> v = {mk(0, 0, 1, 2), mk(1, 1, 1, 1)};
        Metrics m = simulate(v, 2);
        CHECK(approx(m.bufferUtilization(), 1.0 / (3.0 * 2.0)), "time-averaged buffer utilization");
    }
    {  // conservation law
        Metrics m = simulate(generatePackets(20000, 200, 5), 8);
        CHECK(m.arrived == m.processed + m.dropped && m.arrived == 20000, "arrived = processed + dropped");
    }
    {  // higher arrival rate -> more drops
        double d1 = simulate(generatePackets(50000, 100, 1), 20).dropRate();
        double d2 = simulate(generatePackets(50000, 150, 1), 20).dropRate();
        double d3 = simulate(generatePackets(50000, 200, 1), 20).dropRate();
        CHECK(d1 <= d2 && d2 < d3, "drop rate grows with the arrival rate");
    }
    {  // throughput cannot exceed link capacity
        Metrics m = simulate(generatePackets(50000, 300, 1), 20);
        CHECK(m.throughput() <= SERVICE_RATE * 1.02, "throughput does not exceed link capacity");
    }
    cout << "\n" << g_pass << " passed, " << g_fail << " failed\n";
    return g_fail;
}

//Demo & exp 
void runDemo() {
    cout << "=== Demo trace: 12 packets, lambda = 300 packets/s, buffer = 3 ===\n\n";
    vector<long> order;
    Metrics m = simulate(generatePackets(12, 300, 7), 3, true, &order);
    printMetrics(m, &order);
}


void runExperiments() {
    const size_t B = 20;
    const double lambdas[] = {100, 150, 200};
    const size_t sizes[] = {1000, 10000, 100000};

    cout << fixed << setprecision(2);
    cout << "=== Experiment: effect of the arrival rate (Poisson arrivals) ===\n"
         << "Link " << LINK_BPS / 1e6 << " Mbps, packet sizes 64-1500 bytes -> router capacity ~"
         << SERVICE_RATE << " packets/s, buffer capacity B = " << B << "\n\n"
         << setw(9) << "lambda" << setw(7) << "rho" << setw(9) << "N" << setw(9) << "drop%"
         << setw(12) << "wait(ms)" << setw(11) << "bufUtil%" << setw(14) << "thr(pkt/s)" << "\n";
    for (double lam : lambdas) {
        for (size_t n : sizes) {
            Metrics m = simulate(generatePackets(n, lam, 42), B);
            cout << setw(9) << lam << setw(7) << lam / SERVICE_RATE << setw(9) << n
                 << setw(9) << 100 * m.dropRate() << setw(12) << m.avgWait() * 1000
                 << setw(11) << 100 * m.bufferUtilization() << setw(14) << m.throughput() << "\n";
        }
        cout << "\n";
    }
}

// CSV 

static const char* CSV_HEADER =
    "lambda_pps,buffer,packets,processed,dropped,drop_pct,avg_wait_ms,buffer_util_pct,throughput_pps\n";

static void csvRow(ofstream& out, double lam, size_t B, const Metrics& m) {
    out << lam << "," << B << "," << m.arrived << "," << m.processed << "," << m.dropped << ","
        << 100 * m.dropRate() << "," << m.avgWait() * 1000 << "," << 100 * m.bufferUtilization()
        << "," << m.throughput() << "\n";
}

static string currentFolder() {
    char buf[4096];
    return GETCWD(buf, sizeof buf) ? string(buf) : string("(unknown folder)");
}

void runCsv(size_t n, double fixedLambda, size_t fixedBuffer) {
    if (n == 0 || fixedLambda <= 0) {
        cerr << "csv needs packets > 0 and arrival rate > 0\n";
        return;
    }
    const double lambdas[] = {50, 100, 150, 200, 250, 300};
    const size_t caps[] = {1, 2, 5, 10, 20, 50, 100, 200};

    ofstream byBuf("buffer_vs_drop.csv"), byLam("arrival_vs_drop.csv");
    if (!byBuf || !byLam) {
        cerr << "could not create the CSV files in " << currentFolder() << "\n";
        return;
    }
    byBuf << fixed << setprecision(4) << CSV_HEADER;
    byLam << fixed << setprecision(4) << CSV_HEADER;

    for (size_t B : caps)  // same packet stream every time, only the buffer changes
        csvRow(byBuf, fixedLambda, B, simulate(generatePackets(n, fixedLambda, 42), B));
    for (double lam : lambdas)
        csvRow(byLam, lam, fixedBuffer, simulate(generatePackets(n, lam, 42), fixedBuffer));

    byBuf.close();
    byLam.close();
    cout << fixed << setprecision(0);
    cout << "=== CSV files ===\n"
         << "Folder: " << currentFolder() << "\n"
         << "Wrote buffer_vs_drop.csv   (arrival rate " << fixedLambda << " pkt/s, buffer varied)\n"
         << "Wrote arrival_vs_drop.csv  (buffer " << fixedBuffer << ", arrival rate varied)\n"
         << "Packets per run: " << n << "\n";
}

int main(int argc, char** argv) {
    string mode = argc > 1 ? argv[1] : "all";
    if (mode == "test") return runTests() ? 1 : 0;
    if (mode == "demo") { runDemo(); return 0; }
    if (mode == "exp") { runExperiments(); return 0; }
    if (mode == "csv") {
        long n = argc > 2 ? atol(argv[2]) : 10000;
        double lam = argc > 3 ? atof(argv[3]) : 150;
        long B = argc > 4 ? atol(argv[4]) : 20;
        if (n <= 0 || lam <= 0 || B < 1) {
            cerr << "usage: packet_sim_basic csv [packets>0] [arrival_rate>0] [buffer>=1]\n";
            return 2;
        }
        runCsv((size_t)n, lam, (size_t)B);
        return 0;
    }
    if (mode == "file") {
        if (argc < 4) { cerr << "usage: packet_sim_basic file <path> <capacity>\n"; return 2; }
        long cap = atol(argv[3]);
        vector<Packet> pk = loadFile(argv[2]);
        if (pk.empty() || cap < 0) { cerr << "no valid packets or bad capacity\n"; return 2; }
        vector<long> order;
        Metrics m = simulate(pk, (size_t)cap, true, &order);
        printMetrics(m, &order);
        return 0;
    }
    int fails = runTests();
    cout << "\n";
    runDemo();
    cout << "\n";
    runExperiments();
    runCsv(10000, 150, 20); 
    return fails ? 1 : 0;
}