#include "cpu.h"
#include "mutex.h"
#include "cv.h"
#include "disk.h"
#include "thread.h"
#include <string>
#include <vector>
#include <queue>
#include <iostream>
#include <fstream>
#include <algorithm>

mutex qutex;
cv requester_cv, servicer_cv;
std::vector<thread> threads;
std::vector<std::string> disk_filenames;
std::vector<std::pair<int, int>> disk_queue; // first = requester (ID), second = track (value)
int max_disk_queue;
int num_live_requesters;

struct request_has_id {
    request_has_id(int const& id_in) : id(id_in) { }  
    bool operator () (std::pair<int, int> const& p) { return p.first == id; }
private:
    int id;
};

// Given disk_id, returns 1 if requestor disk_id has a request still in the disk queue
bool request_in_queue(int disk_id) {
    auto it = std::find_if(disk_queue.begin(), disk_queue.end(), request_has_id(disk_id));
    return !(it == disk_queue.end());
}

// Returns the disk_queue index of the closest track w.r.t current_track
int find_closest_track(int current_track) {
    int closest_track_idx = -1;
    int min_diff = 2147483647;
    for (size_t i = 0; i < disk_queue.size(); i++) {
        if (std::abs(current_track - disk_queue[i].second) < min_diff) {
            min_diff = std::abs(current_track - disk_queue[i].second);
            closest_track_idx = i;
        }
    }
    return closest_track_idx;
}

void requester_func(void *a) {
    auto disk_id = (unsigned int) reinterpret_cast<intptr_t>(a);
    std::queue<int> input_queue;
    std::string track;
    std::string filename = disk_filenames[disk_id];
    std::ifstream fin(filename);
    // Read disk and put into input_queue
    while (std::getline(fin, track)) {
        input_queue.push(std::stoi(track));
    }
    // While there are still requests to be requested
    while (!input_queue.empty()) {
        qutex.lock();
        // While prev. request still in queue OR queue is full, release lock and sleep
        while (request_in_queue(disk_id) || disk_queue.size() >= std::min(max_disk_queue, num_live_requesters)) {
            requester_cv.wait(qutex);
        }
        // Print disk request
        print_request(disk_id, input_queue.front());
        disk_queue.push_back(std::make_pair((int) disk_id, input_queue.front())); // Push request into shared req. queue
        input_queue.pop();
        servicer_cv.signal(); // Wake up servicer
        qutex.unlock();
    }
    qutex.lock();
    while (request_in_queue(disk_id)) {requester_cv.wait(qutex);}
    num_live_requesters--; // this is a shared resource, might need to gabagool
    servicer_cv.signal();
    qutex.unlock();
}

void servicer_func(void *a) {
    int current_track = 0;
    while (num_live_requesters > 0) {
        qutex.lock();
        while (disk_queue.empty() || disk_queue.size() < std::min(max_disk_queue, num_live_requesters)) {
            servicer_cv.wait(qutex); // Have a requester wake this up
        }
        if (!disk_queue.empty()) {
            int idx = find_closest_track(current_track);
            print_service(disk_queue[idx].first, disk_queue[idx].second);
            current_track = disk_queue[idx].second; // OOPS!
            disk_queue.erase(disk_queue.begin() + idx);
            requester_cv.broadcast(); // Wake up all requestors
        }
        qutex.unlock();
    }
}

void scheduler(void *a) {
    qutex.lock();
    // Initialize servicer thread
    threads.push_back(thread(servicer_func, reinterpret_cast<void *>(0)));
    // Initialize requestor threads
    for (size_t i = 0; i < disk_filenames.size(); i++) {
        threads.push_back(thread(requester_func, reinterpret_cast<void *>(i)));
    }
    qutex.unlock();
    // Join requester & servicer threads when finished
    for (size_t i = 0; i < threads.size(); i++) {
        threads[i].join();
    }
}

int main(int argc, char* argv[]) {
    std::ios_base::sync_with_stdio(false);
    // Read in proper data
    max_disk_queue = std::atoi(argv[1]);
    for (int i = 2; i < argc; i++) {
        disk_filenames.push_back(argv[i]);
        num_live_requesters++;
    }
    cpu::boot(scheduler, reinterpret_cast<void *>(0), 0);
    return 0;
}
