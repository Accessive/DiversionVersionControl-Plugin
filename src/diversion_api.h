#pragma once

#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/string.hpp>

namespace diversion {

// One other-workspace edit of a file (from the other_statuses endpoint).
struct OtherEdit {
	godot::String path; // workspace-relative
	godot::String author; // display name / email
	godot::String branch_name;
	godot::String workspace_id;
	int status = 0; // 1=INTACT 2=ADDED 3=MODIFIED 4=DELETED
};

// Talks to the Diversion cloud REST API. Handles the AWS Cognito
// refresh-token -> access-token exchange (the API token users generate in the
// web app is a Cognito refresh token, not a usable bearer) and caches the
// short-lived access token, re-exchanging on expiry or 401.
//
// Uses Godot's HTTPClient/JSON only, so it links no extra dependencies. All
// calls are blocking and meant to run on a background thread.
class DiversionApi {
public:
	// The API token (Cognito refresh token) from the web app. Setting a
	// different token invalidates any cached access token.
	void set_refresh_token(const godot::String &p_token);
	bool has_token() const { return !refresh_token.is_empty(); }

	// Cognito user-pool region and app client id. Defaults match Diversion's
	// production pool but stay configurable since it is uncontracted.
	void set_region(const godot::String &p_region) { region = p_region; }
	void set_client_id(const godot::String &p_client_id) { client_id = p_client_id; }

	// Fetches other workspaces'/branches' edits for the given repo+workspace.
	// Returns true on success (r_edits filled, possibly empty). On failure
	// r_error carries a human-readable reason; r_auth_invalid is set when the
	// refresh token itself was rejected (user must regenerate it).
	bool fetch_other_statuses(const godot::String &repo_id, const godot::String &workspace_id,
			godot::Vector<OtherEdit> &r_edits, godot::String &r_error, bool &r_auth_invalid);

private:
	godot::String refresh_token;
	godot::String access_token;
	int64_t access_expiry_unix = 0;

	godot::String region = "us-east-2";
	godot::String client_id = "j084768v4hd6j1pf8df4h4c47";

	// Ensures a non-expired access token, exchanging the refresh token if
	// needed. Returns false on failure; sets r_auth_invalid when the refresh
	// token was rejected.
	bool ensure_access_token(godot::String &r_error, bool &r_auth_invalid);

	// Exchanges refresh_token for a fresh access_token via Cognito InitiateAuth.
	bool exchange_refresh_token(godot::String &r_error, bool &r_auth_invalid);
};

} // namespace diversion
