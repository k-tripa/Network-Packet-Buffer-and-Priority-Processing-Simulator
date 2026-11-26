#include<iostream>
#include<vector>
#include<string>

using namespace std;

// ---------------------------------------------------------------- Packet ----
struct Packet {
    long id = 0;          // arrival index (for display)
    double arrival = 0;   // arrival time
    int priority = 0;     // 0 = highest
    int size = 0;         // bytes
    double service = 1;   // transmission time
    long seq = 0;         // assigned on arrival, used for FIFO tie-break
};

struct PacketOrder {  // "a is served before b"
    bool operator()(const Packet& a, const Packet& b) const {
        if (a.priority != b.priority) return a.priority < b.priority;
        return a.seq < b.seq;
    }
};