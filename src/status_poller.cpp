#include "status_poller.h"

#include <chrono>

using namespace godot;

namespace diversion {

// Kept short so the Commit dock reflects saves/agent changes promptly. The
// poll runs on a background thread, so a tight interval doesn't stall the UI.
static const std::chrono::milliseconds POLL_INTERVAL(1000);

StatusPoller::~StatusPoller() {
	stop();
}

void StatusPoller::start(const String &p_workspace_dir) {
	stop();
	{
		std::lock_guard<std::mutex> lock(mutex);
		workspace_dir = p_workspace_dir;
		exit_requested = false;
		has_snapshot = false;
		fail_count = 0;
	}
	worker = std::thread(&StatusPoller::thread_loop, this);
}

void StatusPoller::stop() {
	{
		std::lock_guard<std::mutex> lock(mutex);
		exit_requested = true;
	}
	cv.notify_all();
	if (worker.joinable()) {
		worker.join();
	}
}

DvStatusInfo StatusPoller::get_snapshot() {
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (has_snapshot) {
			return snapshot;
		}
	}
	poll_now();
	std::lock_guard<std::mutex> lock(mutex);
	return snapshot;
}

void StatusPoller::poll_now() {
	poll_once();
}

bool StatusPoller::is_failing(String &r_message) {
	std::lock_guard<std::mutex> lock(mutex);
	r_message = fail_message;
	return fail_count > 0;
}

void StatusPoller::thread_loop() {
	while (true) {
		poll_once();
		std::unique_lock<std::mutex> lock(mutex);
		cv.wait_for(lock, POLL_INTERVAL, [this] { return exit_requested; });
		if (exit_requested) {
			return;
		}
	}
}

void StatusPoller::poll_once() {
	String dir;
	{
		std::lock_guard<std::mutex> lock(mutex);
		dir = workspace_dir;
	}
	if (dir.is_empty()) {
		return;
	}

	// The subprocess runs without holding the lock.
	SubprocessResult res = DvCli::run(dir, dv_args({ "status", "--no-limit", "--nowait" }));

	std::lock_guard<std::mutex> lock(mutex);
	if (res.exit_code != 0) {
		fail_count++;
		fail_message = res.output.strip_edges();
		return; // Keep the last good snapshot.
	}
	DvStatusInfo parsed = parse_dv_status(res.output);
	if (parsed.valid) {
		snapshot = parsed;
		has_snapshot = true;
		fail_count = 0;
		fail_message = String();
	}
}

} // namespace diversion
