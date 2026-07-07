#pragma once

#include "dv_cli.h"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace diversion {

// Polls `dv status` on a background thread so the editor's frequent
// synchronous status queries never block the UI on a subprocess. The plugin
// reads the latest snapshot; mutating operations call poll_now() to refresh
// synchronously before the dock re-queries.
class StatusPoller {
public:
	~StatusPoller();

	void start(const godot::String &workspace_dir);
	void stop();

	// Latest parsed status. Performs a blocking poll if none exists yet.
	DvStatusInfo get_snapshot();

	// Runs one poll on the calling thread and updates the snapshot.
	void poll_now();

	// True while dv status keeps failing; fills the last error output.
	bool is_failing(godot::String &r_message);

private:
	void thread_loop();
	void poll_once();

	std::thread worker;
	std::mutex mutex;
	std::condition_variable cv;
	bool exit_requested = false;

	godot::String workspace_dir;
	DvStatusInfo snapshot;
	bool has_snapshot = false;
	int fail_count = 0;
	godot::String fail_message;
};

} // namespace diversion
