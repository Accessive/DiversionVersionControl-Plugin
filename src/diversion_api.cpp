#include "diversion_api.h"

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
	String url = "https://cognito-idp." + region + ".amazonaws.com/";

	PackedStringArray headers;
	headers.push_back("Content-Type: application/x-amz-json-1.1");
	headers.push_back("X-Amz-Target: AWSCognitoIdentityProviderService.InitiateAuth");

	Dictionary auth_params;
	auth_params["REFRESH_TOKEN"] = refresh_token;
	Dictionary payload;
	payload["AuthFlow"] = "REFRESH_TOKEN_AUTH";
	payload["ClientId"] = client_id;
	payload["AuthParameters"] = auth_params;
	String body = JSON::stringify(payload);

	HttpResponse res = https_request(HTTPClient::METHOD_POST, url, headers, body);
	if (res.code < 0) {
		r_error = "Could not reach Diversion authentication: " + res.error;
		return false;
	}

	Variant parsed = JSON::parse_string(res.body);
	Dictionary obj = parsed;

	if (res.code != 200) {
		String type = obj.get("__type", "");
		// NotAuthorized / InvalidGrant mean the refresh token is dead.
		if (type.contains("NotAuthorized") || type.contains("InvalidParameter") || type.contains("UserNotFound")) {
			r_auth_invalid = true;
			r_error = "Your Diversion API token was rejected. Generate a new one in the Diversion web app (Avatar -> Integrations) and re-enter it.";
		} else {
			r_error = "Authentication failed (" + itos(res.code) + " " + type + ")";
		}
		return false;
	}

	Dictionary result = obj.get("AuthenticationResult", Dictionary());
	String token = result.get("AccessToken", "");
	if (token.is_empty()) {
		r_error = "Authentication response had no access token.";
		return false;
	}
	int expires_in = (int)result.get("ExpiresIn", 3600);

	access_token = token;
	// Refresh a minute early to avoid racing expiry mid-request.
	access_expiry_unix = Time::get_singleton()->get_unix_time_from_system() + expires_in - 60;
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
			Variant author = fs.get("author", Variant());
			if (author.get_type() == Variant::DICTIONARY) {
				Dictionary a = author;
				edit.author = a.get("name", a.get("email", "someone"));
			} else {
				edit.author = author;
			}
			r_edits.push_back(edit);
		}
	}
	return true;
}

} // namespace diversion
