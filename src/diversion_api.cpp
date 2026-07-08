#include "diversion_api.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/http_client.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/tls_options.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace diversion {

namespace {

struct HttpResponse {
	int code = -1; // -1 = transport failure (never reached the server)
	String body;
	String error;
};

// Blocking HTTPS request via Godot's HTTPClient. url must be https://host/path.
HttpResponse https_request(HTTPClient::Method method, const String &url,
		const PackedStringArray &headers, const String &body) {
	HttpResponse out;

	String rest = url.trim_prefix("https://");
	int slash = rest.find("/");
	String host = slash < 0 ? rest : rest.substr(0, slash);
	String path = slash < 0 ? String("/") : rest.substr(slash);

	Ref<HTTPClient> http;
	http.instantiate();
	Ref<TLSOptions> tls = TLSOptions::client();

	Error err = http->connect_to_host(host, 443, tls);
	if (err != OK) {
		out.error = "connect_to_host failed";
		return out;
	}

	// Pump the connection handshake.
	while (true) {
		HTTPClient::Status status = http->get_status();
		if (status == HTTPClient::STATUS_CONNECTED) {
			break;
		}
		if (status == HTTPClient::STATUS_CONNECTING || status == HTTPClient::STATUS_RESOLVING) {
			http->poll();
			OS::get_singleton()->delay_msec(5);
			continue;
		}
		out.error = "connection failed (status " + itos(status) + ")";
		return out;
	}

	err = http->request(method, path, headers, body);
	if (err != OK) {
		out.error = "request failed";
		return out;
	}

	while (http->get_status() == HTTPClient::STATUS_REQUESTING) {
		http->poll();
		OS::get_singleton()->delay_msec(5);
	}

	if (!http->has_response()) {
		out.error = "no response";
		return out;
	}

	out.code = http->get_response_code();

	PackedByteArray raw;
	while (http->get_status() == HTTPClient::STATUS_BODY) {
		http->poll();
		PackedByteArray chunk = http->read_response_body_chunk();
		if (chunk.size() > 0) {
			raw.append_array(chunk);
		} else {
			OS::get_singleton()->delay_msec(5);
		}
	}
	out.body = raw.get_string_from_utf8();
	return out;
}

} // namespace

void DiversionApi::set_refresh_token(const String &p_token) {
	if (p_token == refresh_token) {
		return;
	}
	refresh_token = p_token;
	access_token = String();
	access_expiry_unix = 0;
}

bool DiversionApi::exchange_refresh_token(String &r_error, bool &r_auth_invalid) {
	// Standard OAuth2 refresh-token grant against Cognito's Hosted-UI token
	// endpoint (what the official Diversion plugins use), form-url-encoded.
	PackedStringArray headers;
	headers.push_back("Content-Type: application/x-www-form-urlencoded");
	String body = "grant_type=refresh_token&refresh_token=" + refresh_token.uri_encode() +
			"&client_id=" + client_id.uri_encode();

	HttpResponse res = https_request(HTTPClient::METHOD_POST, oauth_url, headers, body);
	if (res.code < 0) {
		r_error = "Could not reach Diversion authentication: " + res.error;
		return false;
	}

	Variant parsed = JSON::parse_string(res.body);
	Dictionary obj = parsed;

	if (res.code != 200) {
		String err = obj.get("error", "");
		// invalid_grant means the stored login is dead; the user must re-login.
		if (err == "invalid_grant" || res.code == 400) {
			r_auth_invalid = true;
			r_error = "Diversion login expired or invalid. Run 'dv login' again to re-enable lock alerts.";
		} else {
			r_error = "Authentication failed (" + itos(res.code) + " " + err + ")";
		}
		return false;
	}

	String token = obj.get("access_token", "");
	if (token.is_empty()) {
		r_error = "Authentication response had no access token.";
		return false;
	}
	int expires_in = (int)obj.get("expires_in", 3600);

	access_token = token;
	// Refresh a minute early to avoid racing expiry mid-request.
	access_expiry_unix = Time::get_singleton()->get_unix_time_from_system() + expires_in - 60;
	return true;
}

bool DiversionApi::load_local_credentials(String &r_error) {
	String home = OS::get_singleton()->get_environment("USERPROFILE");
	if (home.is_empty()) {
		home = OS::get_singleton()->get_environment("HOME");
	}
	String cred_dir = home.path_join(".diversion").path_join("credentials");
	if (!DirAccess::dir_exists_absolute(cred_dir)) {
		r_error = "Not logged in to Diversion (no credentials found). Run 'dv login'.";
		return false;
	}

	// The folder holds one JSON file per logged-in user; pick the most
	// recently modified, matching whichever account is active.
	Ref<DirAccess> dir = DirAccess::open(cred_dir);
	if (dir.is_null()) {
		r_error = "Could not read Diversion credentials directory.";
		return false;
	}
	String best_file;
	int64_t best_mtime = -1;
	dir->list_dir_begin();
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		if (dir->current_is_dir()) {
			continue;
		}
		String full = cred_dir.path_join(name);
		int64_t mtime = (int64_t)FileAccess::get_modified_time(full);
		if (mtime > best_mtime) {
			best_mtime = mtime;
			best_file = full;
		}
	}
	dir->list_dir_end();

	if (best_file.is_empty()) {
		r_error = "Not logged in to Diversion (no credentials found). Run 'dv login'.";
		return false;
	}

	String contents = FileAccess::get_file_as_string(best_file);
	Variant parsed = JSON::parse_string(contents);
	Dictionary obj = parsed;
	Dictionary token = obj.get("token", Dictionary());
	String rt = token.get("refresh_token", "");
	if (rt.is_empty()) {
		r_error = "Diversion credentials had no refresh token. Run 'dv login' again.";
		return false;
	}
	set_refresh_token(rt);
	return true;
}

bool DiversionApi::ensure_access_token(String &r_error, bool &r_auth_invalid) {
	if (refresh_token.is_empty()) {
		r_error = "No Diversion API token set.";
		return false;
	}
	int64_t now = Time::get_singleton()->get_unix_time_from_system();
	if (!access_token.is_empty() && now < access_expiry_unix) {
		return true;
	}
	return exchange_refresh_token(r_error, r_auth_invalid);
}

bool DiversionApi::fetch_other_statuses(const String &repo_id, const String &workspace_id,
		Vector<OtherEdit> &r_edits, String &r_error, bool &r_auth_invalid) {
	r_auth_invalid = false;
	if (!ensure_access_token(r_error, r_auth_invalid)) {
		return false;
	}

	String url = "https://api.diversion.dev/v0/repos/" + repo_id + "/workspaces/" + workspace_id + "/other_statuses?limit=1000";
	PackedStringArray headers;
	headers.push_back("Authorization: Bearer " + access_token);

	HttpResponse res = https_request(HTTPClient::METHOD_GET, url, headers, String());

	// A cached access token can still expire early; retry once after a forced
	// re-exchange on 401.
	if (res.code == 401) {
		access_token = String();
		access_expiry_unix = 0;
		if (!ensure_access_token(r_error, r_auth_invalid)) {
			return false;
		}
		headers.clear();
		headers.push_back("Authorization: Bearer " + access_token);
		res = https_request(HTTPClient::METHOD_GET, url, headers, String());
	}

	if (res.code < 0) {
		r_error = "Could not reach Diversion API: " + res.error;
		return false;
	}
	if (res.code != 200) {
		r_error = "other_statuses returned HTTP " + itos(res.code);
		return false;
	}

	Variant parsed = JSON::parse_string(res.body);
	Dictionary obj = parsed;
	Array statuses = obj.get("statuses", Array());
	for (int i = 0; i < statuses.size(); i++) {
		Dictionary entry = statuses[i];
		String path = entry.get("path", "");
		Array file_statuses = entry.get("file_statuses", Array());
		for (int j = 0; j < file_statuses.size(); j++) {
			Dictionary fs = file_statuses[j];
			OtherEdit edit;
			edit.path = path;
			edit.status = (int)fs.get("status", 0);
			edit.branch_name = fs.get("branch_name", fs.get("branch_id", ""));
			edit.workspace_id = fs.get("workspace_id", "");
			edit.mtime = (int64_t)fs.get("mtime", -1);
			Variant author = fs.get("author", Variant());
			if (author.get_type() == Variant::DICTIONARY) {
				Dictionary a = author;
				edit.author = a.get("full_name", a.get("name", a.get("email", "someone")));
			} else {
				edit.author = author;
			}
			r_edits.push_back(edit);
		}
	}
	return true;
}

} // namespace diversion
