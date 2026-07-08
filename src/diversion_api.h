#pragma once

#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/string.hpp>

namespace diversion {

// One other-workspace edit of a file (from the other_statuses endpoint).
struct OtherEdit {
	godot::String path; // workspace-relative
	godot::String author; // display name / email
	godot::String branch_name;
	godot::String workspace_id; // empty => the change was already committed
	int status = 0; // 1=INTACT 2=ADDED 3=MODIFIED 4=DELETED
	int64_t mtime = -1; // unix seconds of the other edit, -1 if unknown
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
	// Sets the Cognito refresh token used to mint access tokens. Setting a
	// different token invalidates any cached access token.
	void set_refresh_token(const godot::String &p_token);
	bool has_token() const { return !refresh_token.is_empty(); }

	// Loads the refresh token from the local Diversion credentials that
	// `dv login` writes (~/.diversion/credentials/<user>), the same source the
	// official Unreal plugin reads. Returns true if a token was found. This is
	// the zero-config path: the user only needs to be logged in via dv.
	bool load_local_credentials(godot::String &r_error);

	// OAuth token endpoint and app client id. Defaults match Diversion's
	// production login client (from the official plugin) but stay configurable
	// since the flow is uncontracted.
	void set_oauth_url(const godot::String &p_url) { oauth_url = p_url; }
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

	// Branded Cognito Hosted-UI token endpoint + the login client id the
	// official Diversion plugins use. More stable than the raw regional
	// cognito-idp endpoint (no hardcoded region).
	godot::String oauth_url = "https://auth.diversion.dev/oauth2/token";
	godot::String client_id = "nmm65ta2r48pvj1lsjcmoeb7l";

	// Ensures a non-expired access token, exchanging the refresh token if
	// needed. Returns false on failure; sets r_auth_invalid when the refresh
	// token was rejected.
	bool ensure_access_token(godot::String &r_error, bool &r_auth_invalid);

	// Exchanges refresh_token for a fresh access_token via Cognito InitiateAuth.
	bool exchange_refresh_token(godot::String &r_error, bool &r_auth_invalid);
};

} // namespace diversion
