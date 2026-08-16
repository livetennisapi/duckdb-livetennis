#define DUCKDB_EXTENSION_MAIN

#include "livetennis_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"
#include "yyjson.hpp"

#include <cstdlib>
#include <functional>

namespace duckdb {

using namespace duckdb_yyjson; // NOLINT(*-build-using-namespace): DuckDB's vendored yyjson

//===--------------------------------------------------------------------===//
// Configuration (API key + base URL) resolution
//===--------------------------------------------------------------------===//

static constexpr const char *DEFAULT_BASE_URL = "https://api.livetennisapi.com/api/public/v1";
static constexpr const char *SECRET_TYPE = "livetennis";
static constexpr idx_t PAGE_LIMIT = 200; // the API's maximum page size
static constexpr idx_t MAX_PAGES = 1000; // internal safety bound on pagination

struct LiveTennisConfig {
	string api_key;
	string base_url = DEFAULT_BASE_URL;
};

static void TryReadFromSecret(ClientContext &context, LiveTennisConfig &config) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto match = secret_manager.LookupSecret(transaction, "livetennis://", SECRET_TYPE);
	if (!match.HasMatch()) {
		return;
	}
	const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(match.GetSecret());
	Value value;
	if (kv_secret.TryGetValue("api_key", value) && !value.IsNull()) {
		config.api_key = value.ToString();
	}
	if (kv_secret.TryGetValue("base_url", value) && !value.IsNull() && !value.ToString().empty()) {
		config.base_url = value.ToString();
	}
}

static LiveTennisConfig ResolveConfig(ClientContext &context) {
	LiveTennisConfig config;

	// 1. DuckDB secret (CREATE SECRET (TYPE livetennis, API_KEY '...'))
	TryReadFromSecret(context, config);

	// 2. Extension settings
	Value setting;
	if (config.api_key.empty() && context.TryGetCurrentSetting("livetennis_api_key", setting) && !setting.IsNull() &&
	    !setting.ToString().empty()) {
		config.api_key = setting.ToString();
	}
	if (config.base_url == DEFAULT_BASE_URL && context.TryGetCurrentSetting("livetennis_base_url", setting) &&
	    !setting.IsNull() && !setting.ToString().empty()) {
		config.base_url = setting.ToString();
	}

	// 3. Environment variables
	if (config.api_key.empty()) {
		auto *env_key = std::getenv("LIVE_TENNIS_API_KEY");
		if (env_key && *env_key) {
			config.api_key = env_key;
		}
	}
	if (config.base_url == DEFAULT_BASE_URL) {
		auto *env_url = std::getenv("LIVE_TENNIS_BASE_URL");
		if (env_url && *env_url) {
			config.base_url = env_url;
		}
	}

	if (config.api_key.empty()) {
		throw InvalidInputException(
		    "No Live Tennis API key configured. Provide one with CREATE SECRET (TYPE livetennis, API_KEY '...'), "
		    "SET livetennis_api_key = '...', or the LIVE_TENNIS_API_KEY environment variable. "
		    "A free key is available at https://livetennisapi.com/subscribe/free");
	}
	return config;
}

//===--------------------------------------------------------------------===//
// HTTP
//===--------------------------------------------------------------------===//

static string UrlEncode(const string &input) {
	static const char *HEX_DIGITS = "0123456789ABCDEF";
	string result;
	for (unsigned char c : input) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
		    c == '.' || c == '~') {
			result += static_cast<char>(c);
		} else {
			result += '%';
			result += HEX_DIGITS[c >> 4];
			result += HEX_DIGITS[c & 0x0F];
		}
	}
	return result;
}

//! Split "https://host[:port]/prefix" into origin ("https://host[:port]") and path prefix ("/prefix")
static void SplitBaseUrl(const string &base_url, string &origin, string &path_prefix) {
	auto scheme_end = base_url.find("://");
	if (scheme_end == string::npos) {
		throw InvalidInputException("Invalid Live Tennis API base URL '%s': expected http:// or https://", base_url);
	}
	auto path_start = base_url.find('/', scheme_end + 3);
	if (path_start == string::npos) {
		origin = base_url;
		path_prefix = "";
	} else {
		origin = base_url.substr(0, path_start);
		path_prefix = base_url.substr(path_start);
	}
	while (!path_prefix.empty() && path_prefix.back() == '/') {
		path_prefix.pop_back();
	}
}

//! Perform a GET against the API; returns the response body on HTTP 200, throws a descriptive error otherwise
static string HttpGetJson(const LiveTennisConfig &config, const string &path_and_query) {
	string origin;
	string path_prefix;
	SplitBaseUrl(config.base_url, origin, path_prefix);
	auto full_path = path_prefix + path_and_query;

	duckdb_httplib_openssl::Client client(origin);
	client.set_connection_timeout(10, 0);
	client.set_read_timeout(30, 0);
	client.set_follow_location(true);

	duckdb_httplib_openssl::Headers headers {{"Authorization", "Bearer " + config.api_key},
	                                         {"Accept", "application/json"}};
	auto res = client.Get(full_path, headers);
	if (!res) {
		throw IOException("Could not reach the Live Tennis API at %s%s: %s", origin, full_path,
		                  duckdb_httplib_openssl::to_string(res.error()));
	}
	if (res->status == 200) {
		return res->body;
	}

	// Try to surface the API's own error message
	string api_error;
	yyjson_doc *doc = yyjson_read(res->body.c_str(), res->body.size(), 0);
	if (doc) {
		auto *root = yyjson_doc_get_root(doc);
		auto *err = yyjson_obj_get(root, "error");
		if (err && yyjson_is_str(err)) {
			api_error = yyjson_get_str(err);
		}
		yyjson_doc_free(doc);
	}

	switch (res->status) {
	case 401:
		throw InvalidInputException("The Live Tennis API rejected the configured key (HTTP 401 unauthorized). "
		                            "Check the key in your livetennis secret, the livetennis_api_key setting, "
		                            "or the LIVE_TENNIS_API_KEY environment variable.");
	case 403:
		throw PermissionException("The Live Tennis API returned HTTP 403 (%s) for GET %s. "
		                          "This usually means the request needs a higher subscription tier: "
		                          "see https://livetennisapi.com/pricing",
		                          api_error.empty() ? "forbidden" : api_error, full_path);
	case 429:
		throw IOException("The Live Tennis API rate limit was exceeded (HTTP 429). FREE keys allow 30 "
		                  "requests/minute and 100/day; higher tiers raise the limits. Retry later.");
	case 400:
		throw InvalidInputException("The Live Tennis API rejected the request (HTTP 400%s%s) for GET %s",
		                            api_error.empty() ? "" : ": ", api_error, full_path);
	default:
		throw IOException("The Live Tennis API returned HTTP %d%s%s for GET %s", res->status,
		                  api_error.empty() ? "" : ": ", api_error, full_path);
	}
}

//===--------------------------------------------------------------------===//
// JSON -> Value helpers (nulls in the payload become SQL NULLs; absent fields too)
//===--------------------------------------------------------------------===//

static bool JsonMissing(yyjson_val *val) {
	return !val || yyjson_is_null(val);
}

static Value JsonToVarchar(yyjson_val *obj, const char *key) {
	auto *val = yyjson_obj_get(obj, key);
	if (JsonMissing(val)) {
		return Value(LogicalType::VARCHAR);
	}
	if (!yyjson_is_str(val)) {
		throw InvalidInputException("Live Tennis API: expected a string for field '%s'", key);
	}
	return Value(yyjson_get_str(val));
}

static Value JsonToBigint(yyjson_val *obj, const char *key) {
	auto *val = yyjson_obj_get(obj, key);
	if (JsonMissing(val)) {
		return Value(LogicalType::BIGINT);
	}
	if (!yyjson_is_int(val)) {
		throw InvalidInputException("Live Tennis API: expected an integer for field '%s'", key);
	}
	return Value::BIGINT(yyjson_get_sint(val));
}

static Value JsonToInteger(yyjson_val *obj, const char *key) {
	auto *val = yyjson_obj_get(obj, key);
	if (JsonMissing(val)) {
		return Value(LogicalType::INTEGER);
	}
	if (!yyjson_is_int(val)) {
		throw InvalidInputException("Live Tennis API: expected an integer for field '%s'", key);
	}
	return Value::INTEGER(static_cast<int32_t>(yyjson_get_sint(val)));
}

static Value JsonToBoolean(yyjson_val *obj, const char *key) {
	auto *val = yyjson_obj_get(obj, key);
	if (JsonMissing(val)) {
		return Value(LogicalType::BOOLEAN);
	}
	if (!yyjson_is_bool(val)) {
		throw InvalidInputException("Live Tennis API: expected a boolean for field '%s'", key);
	}
	return Value::BOOLEAN(yyjson_get_bool(val));
}

static Value JsonToDouble(yyjson_val *obj, const char *key) {
	auto *val = yyjson_obj_get(obj, key);
	if (JsonMissing(val)) {
		return Value(LogicalType::DOUBLE);
	}
	if (!yyjson_is_num(val)) {
		throw InvalidInputException("Live Tennis API: expected a number for field '%s'", key);
	}
	return Value::DOUBLE(yyjson_get_num(val));
}

static Value JsonToTimestampTZ(yyjson_val *obj, const char *key) {
	auto varchar = JsonToVarchar(obj, key);
	if (varchar.IsNull()) {
		return Value(LogicalType::TIMESTAMP_TZ);
	}
	return varchar.DefaultCastAs(LogicalType::TIMESTAMP_TZ);
}

static Value JsonToDate(yyjson_val *obj, const char *key) {
	auto varchar = JsonToVarchar(obj, key);
	if (varchar.IsNull()) {
		return Value(LogicalType::DATE);
	}
	return varchar.DefaultCastAs(LogicalType::DATE);
}

//! [6, 4] -> INTEGER[] (payload null entries stay NULL)
static Value JsonToIntegerList(yyjson_val *arr) {
	if (JsonMissing(arr)) {
		return Value(LogicalType::LIST(LogicalType::INTEGER));
	}
	if (!yyjson_is_arr(arr)) {
		throw InvalidInputException("Live Tennis API: expected an array of integers");
	}
	vector<Value> children;
	size_t idx, max;
	yyjson_val *item;
	yyjson_arr_foreach(arr, idx, max, item) {
		if (yyjson_is_null(item)) {
			children.emplace_back(Value(LogicalType::INTEGER));
		} else if (yyjson_is_int(item)) {
			children.emplace_back(Value::INTEGER(static_cast<int32_t>(yyjson_get_sint(item))));
		} else {
			throw InvalidInputException("Live Tennis API: expected an integer inside an array");
		}
	}
	return Value::LIST(LogicalType::INTEGER, std::move(children));
}

//! [[6, 4], [2, 6]] -> INTEGER[][]
static Value JsonToIntegerListList(yyjson_val *arr) {
	if (JsonMissing(arr)) {
		return Value(LogicalType::LIST(LogicalType::LIST(LogicalType::INTEGER)));
	}
	if (!yyjson_is_arr(arr)) {
		throw InvalidInputException("Live Tennis API: expected an array of arrays");
	}
	vector<Value> children;
	size_t idx, max;
	yyjson_val *item;
	yyjson_arr_foreach(arr, idx, max, item) {
		children.emplace_back(JsonToIntegerList(item));
	}
	return Value::LIST(LogicalType::LIST(LogicalType::INTEGER), std::move(children));
}

//! ["15", "40", null] -> VARCHAR[] (the API documents NULL entries as real states)
static Value JsonToVarcharList(yyjson_val *arr) {
	if (JsonMissing(arr)) {
		return Value(LogicalType::LIST(LogicalType::VARCHAR));
	}
	if (!yyjson_is_arr(arr)) {
		throw InvalidInputException("Live Tennis API: expected an array of strings");
	}
	vector<Value> children;
	size_t idx, max;
	yyjson_val *item;
	yyjson_arr_foreach(arr, idx, max, item) {
		if (yyjson_is_null(item)) {
			children.emplace_back(Value(LogicalType::VARCHAR));
		} else if (yyjson_is_str(item)) {
			children.emplace_back(Value(yyjson_get_str(item)));
		} else {
			throw InvalidInputException("Live Tennis API: expected a string inside an array");
		}
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(children));
}

//===--------------------------------------------------------------------===//
// Paged fetch shared by all three table functions
//===--------------------------------------------------------------------===//

struct LiveTennisScanState : public GlobalTableFunctionState {
	vector<vector<Value>> rows;
	idx_t cursor = 0;

	idx_t MaxThreads() const override {
		return 1;
	}
};

//! Fetches every page of `endpoint` (which may already contain query parameters) and
//! calls `handle_row` for every element of the "data" array.
static void FetchAllPages(ClientContext &context, const string &endpoint,
                          const std::function<void(yyjson_val *)> &handle_row) {
	auto config = ResolveConfig(context);
	idx_t offset = 0;
	for (idx_t page = 0; page < MAX_PAGES; page++) {
		auto *separator = endpoint.find('?') == string::npos ? "?" : "&";
		auto url = StringUtil::Format("%s%slimit=%llu&offset=%llu", endpoint, separator, PAGE_LIMIT, offset);
		auto body = HttpGetJson(config, url);

		yyjson_doc *doc = yyjson_read(body.c_str(), body.size(), 0);
		if (!doc) {
			throw IOException("The Live Tennis API returned a response that is not valid JSON for GET %s", url);
		}
		bool has_more = false;
		idx_t count = 0;
		try {
			auto *root = yyjson_doc_get_root(doc);
			if (!yyjson_is_obj(root)) {
				throw IOException("The Live Tennis API returned an unexpected JSON shape for GET %s", url);
			}
			auto *data = yyjson_obj_get(root, "data");
			if (!yyjson_is_arr(data)) {
				throw IOException("The Live Tennis API response has no 'data' array for GET %s", url);
			}
			size_t idx, max;
			yyjson_val *row;
			yyjson_arr_foreach(data, idx, max, row) {
				handle_row(row);
				count++;
			}
			auto *meta = yyjson_obj_get(root, "meta");
			auto *has_more_val = meta ? yyjson_obj_get(meta, "has_more") : nullptr;
			has_more = has_more_val && yyjson_is_bool(has_more_val) && yyjson_get_bool(has_more_val);
		} catch (...) {
			yyjson_doc_free(doc);
			throw;
		}
		yyjson_doc_free(doc);

		if (!has_more || count == 0) {
			return;
		}
		offset += count;
	}
}

static void EmitRows(LiveTennisScanState &state, DataChunk &output) {
	idx_t emitted = 0;
	while (state.cursor < state.rows.size() && emitted < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.cursor];
		for (idx_t col = 0; col < row.size(); col++) {
			output.SetValue(col, emitted, row[col]);
		}
		state.cursor++;
		emitted++;
	}
	output.SetCardinality(emitted);
}

//===--------------------------------------------------------------------===//
// live_tennis_matches()
//===--------------------------------------------------------------------===//

struct LiveTennisBindData : public TableFunctionData {
	//! endpoint incl. any query parameters (except limit/offset, which paging adds)
	string endpoint;
};

static void AddMatchColumns(vector<LogicalType> &return_types, vector<string> &names) {
	auto add = [&](const string &name, const LogicalType &type) {
		names.push_back(name);
		return_types.push_back(type);
	};
	add("match_id", LogicalType::BIGINT);
	add("tournament", LogicalType::VARCHAR);
	add("tournament_id", LogicalType::VARCHAR);
	add("tour", LogicalType::VARCHAR);
	add("surface", LogicalType::VARCHAR);
	add("indoor", LogicalType::BOOLEAN);
	add("format", LogicalType::VARCHAR);
	add("round", LogicalType::VARCHAR);
	add("round_code", LogicalType::VARCHAR);
	add("status", LogicalType::VARCHAR);
	add("event_status", LogicalType::VARCHAR);
	add("is_doubles", LogicalType::BOOLEAN);
	add("scheduled_time", LogicalType::TIMESTAMP_TZ);
	add("p1_id", LogicalType::BIGINT);
	add("p1_name", LogicalType::VARCHAR);
	add("p1_country", LogicalType::VARCHAR);
	add("p1_ranking", LogicalType::INTEGER);
	add("p2_id", LogicalType::BIGINT);
	add("p2_name", LogicalType::VARCHAR);
	add("p2_country", LogicalType::VARCHAR);
	add("p2_ranking", LogicalType::INTEGER);
	add("sets", LogicalType::LIST(LogicalType::INTEGER));
	add("games", LogicalType::LIST(LogicalType::LIST(LogicalType::INTEGER)));
	add("points", LogicalType::LIST(LogicalType::VARCHAR));
	add("server", LogicalType::INTEGER);
	add("is_tiebreak", LogicalType::BOOLEAN);
	add("win_probability_p1", LogicalType::DOUBLE);
	add("danger", LogicalType::DOUBLE);
	add("score_timestamp", LogicalType::TIMESTAMP_TZ);
}

static vector<Value> MatchToRow(yyjson_val *match) {
	vector<Value> row;
	row.push_back(JsonToBigint(match, "id"));
	row.push_back(JsonToVarchar(match, "tournament"));
	row.push_back(JsonToVarchar(match, "tournament_id"));
	row.push_back(JsonToVarchar(match, "tour"));
	row.push_back(JsonToVarchar(match, "surface"));
	row.push_back(JsonToBoolean(match, "indoor"));
	row.push_back(JsonToVarchar(match, "format"));
	row.push_back(JsonToVarchar(match, "round"));
	row.push_back(JsonToVarchar(match, "round_code"));
	row.push_back(JsonToVarchar(match, "status"));
	row.push_back(JsonToVarchar(match, "event_status"));
	row.push_back(JsonToBoolean(match, "is_doubles"));
	row.push_back(JsonToTimestampTZ(match, "scheduled_time"));

	auto *players = yyjson_obj_get(match, "players");
	for (const auto *side : {"p1", "p2"}) {
		auto *player = players ? yyjson_obj_get(players, side) : nullptr;
		if (JsonMissing(player)) {
			row.emplace_back(Value(LogicalType::BIGINT));
			row.emplace_back(Value(LogicalType::VARCHAR));
			row.emplace_back(Value(LogicalType::VARCHAR));
			row.emplace_back(Value(LogicalType::INTEGER));
		} else {
			row.push_back(JsonToBigint(player, "id"));
			row.push_back(JsonToVarchar(player, "name"));
			row.push_back(JsonToVarchar(player, "country"));
			row.push_back(JsonToInteger(player, "ranking"));
		}
	}

	auto *score = yyjson_obj_get(match, "score");
	if (JsonMissing(score)) {
		row.emplace_back(Value(LogicalType::LIST(LogicalType::INTEGER)));
		row.emplace_back(Value(LogicalType::LIST(LogicalType::LIST(LogicalType::INTEGER))));
		row.emplace_back(Value(LogicalType::LIST(LogicalType::VARCHAR)));
		row.emplace_back(Value(LogicalType::INTEGER));
		row.emplace_back(Value(LogicalType::BOOLEAN));
		row.emplace_back(Value(LogicalType::DOUBLE));
		row.emplace_back(Value(LogicalType::DOUBLE));
		row.emplace_back(Value(LogicalType::TIMESTAMP_TZ));
	} else {
		row.push_back(JsonToIntegerList(yyjson_obj_get(score, "sets")));
		row.push_back(JsonToIntegerListList(yyjson_obj_get(score, "games")));
		row.push_back(JsonToVarcharList(yyjson_obj_get(score, "points")));
		row.push_back(JsonToInteger(score, "server"));
		row.push_back(JsonToBoolean(score, "is_tiebreak"));
		row.push_back(JsonToDouble(score, "win_probability_p1"));
		row.push_back(JsonToDouble(score, "danger"));
		row.push_back(JsonToTimestampTZ(score, "timestamp"));
	}
	return row;
}

static unique_ptr<FunctionData> MatchesBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<LiveTennisBindData>();
	bind_data->endpoint = "/matches?status=live";
	AddMatchColumns(return_types, names);
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> MatchesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<LiveTennisBindData>();
	auto state = make_uniq<LiveTennisScanState>();
	FetchAllPages(context, bind_data.endpoint, [&](yyjson_val *row) { state->rows.push_back(MatchToRow(row)); });
	return std::move(state);
}

//===--------------------------------------------------------------------===//
// live_tennis_fixtures([tour])
//===--------------------------------------------------------------------===//

static void AddFixtureColumns(vector<LogicalType> &return_types, vector<string> &names) {
	auto add = [&](const string &name, const LogicalType &type) {
		names.push_back(name);
		return_types.push_back(type);
	};
	add("fixture_id", LogicalType::BIGINT);
	add("event_date", LogicalType::DATE);
	add("start_time", LogicalType::TIMESTAMP_TZ);
	add("tour", LogicalType::VARCHAR);
	add("tournament", LogicalType::VARCHAR);
	add("round", LogicalType::VARCHAR);
	add("round_code", LogicalType::VARCHAR);
	add("surface", LogicalType::VARCHAR);
	add("status", LogicalType::VARCHAR);
	add("player1_id", LogicalType::BIGINT);
	add("player1_name", LogicalType::VARCHAR);
	add("player2_id", LogicalType::BIGINT);
	add("player2_name", LogicalType::VARCHAR);
}

static vector<Value> FixtureToRow(yyjson_val *fixture) {
	vector<Value> row;
	row.push_back(JsonToBigint(fixture, "id"));
	row.push_back(JsonToDate(fixture, "event_date"));
	row.push_back(JsonToTimestampTZ(fixture, "start_time"));
	row.push_back(JsonToVarchar(fixture, "tour"));
	row.push_back(JsonToVarchar(fixture, "tournament"));
	row.push_back(JsonToVarchar(fixture, "round"));
	row.push_back(JsonToVarchar(fixture, "round_code"));
	row.push_back(JsonToVarchar(fixture, "surface"));
	row.push_back(JsonToVarchar(fixture, "status"));
	row.push_back(JsonToBigint(fixture, "player1_id"));
	row.push_back(JsonToVarchar(fixture, "player1_name"));
	row.push_back(JsonToBigint(fixture, "player2_id"));
	row.push_back(JsonToVarchar(fixture, "player2_name"));
	return row;
}

static unique_ptr<FunctionData> FixturesBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<LiveTennisBindData>();
	bind_data->endpoint = "/fixtures";
	if (!input.inputs.empty()) {
		if (input.inputs[0].IsNull()) {
			throw InvalidInputException("live_tennis_fixtures: tour argument cannot be NULL; omit it for all tours");
		}
		// Passed through verbatim: the API rejects an unrecognised tour with a clear 400
		bind_data->endpoint += "?tour=" + UrlEncode(input.inputs[0].ToString());
	}
	AddFixtureColumns(return_types, names);
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> FixturesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<LiveTennisBindData>();
	auto state = make_uniq<LiveTennisScanState>();
	FetchAllPages(context, bind_data.endpoint, [&](yyjson_val *row) { state->rows.push_back(FixtureToRow(row)); });
	return std::move(state);
}

//===--------------------------------------------------------------------===//
// live_tennis_players(search)
//===--------------------------------------------------------------------===//

static void AddPlayerColumns(vector<LogicalType> &return_types, vector<string> &names) {
	auto add = [&](const string &name, const LogicalType &type) {
		names.push_back(name);
		return_types.push_back(type);
	};
	add("player_id", LogicalType::BIGINT);
	add("name", LogicalType::VARCHAR);
	add("tour", LogicalType::VARCHAR);
	add("country", LogicalType::VARCHAR);
	add("ranking", LogicalType::INTEGER);
	add("ranking_points", LogicalType::INTEGER);
	add("ranking_movement", LogicalType::VARCHAR);
	add("hand", LogicalType::VARCHAR);
	add("backhand", LogicalType::INTEGER);
	add("birthday", LogicalType::DATE);
	add("is_doubles_team", LogicalType::BOOLEAN);
}

static vector<Value> PlayerToRow(yyjson_val *player) {
	vector<Value> row;
	row.push_back(JsonToBigint(player, "id"));
	row.push_back(JsonToVarchar(player, "name"));
	row.push_back(JsonToVarchar(player, "tour"));
	row.push_back(JsonToVarchar(player, "country"));
	row.push_back(JsonToInteger(player, "ranking"));
	row.push_back(JsonToInteger(player, "ranking_points"));
	row.push_back(JsonToVarchar(player, "ranking_movement"));
	row.push_back(JsonToVarchar(player, "hand"));
	row.push_back(JsonToInteger(player, "backhand"));
	row.push_back(JsonToDate(player, "birthday"));
	row.push_back(JsonToBoolean(player, "is_doubles_team"));
	return row;
}

static unique_ptr<FunctionData> PlayersBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<LiveTennisBindData>();
	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("live_tennis_players: search argument cannot be NULL");
	}
	bind_data->endpoint = "/players?search=" + UrlEncode(input.inputs[0].ToString());
	AddPlayerColumns(return_types, names);
	return std::move(bind_data);
}

static unique_ptr<GlobalTableFunctionState> PlayersInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<LiveTennisBindData>();
	auto state = make_uniq<LiveTennisScanState>();
	FetchAllPages(context, bind_data.endpoint, [&](yyjson_val *row) { state->rows.push_back(PlayerToRow(row)); });
	return std::move(state);
}

//===--------------------------------------------------------------------===//
// Shared scan
//===--------------------------------------------------------------------===//

static void LiveTennisScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<LiveTennisScanState>();
	EmitRows(state, output);
}

//===--------------------------------------------------------------------===//
// Secret type
//===--------------------------------------------------------------------===//

static unique_ptr<BaseSecret> CreateLiveTennisSecret(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	if (scope.empty()) {
		scope.emplace_back("livetennis://");
	}
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	for (const auto &option : input.options) {
		auto lower = StringUtil::Lower(option.first);
		if (lower != "api_key" && lower != "base_url") {
			throw InvalidInputException("Unknown parameter '%s' for livetennis secret; expected API_KEY or BASE_URL",
			                            option.first);
		}
		secret->secret_map[lower] = option.second;
	}
	if (secret->secret_map.find("api_key") == secret->secret_map.end()) {
		throw InvalidInputException("A livetennis secret requires an API_KEY parameter");
	}
	secret->redact_keys.insert("api_key");
	return std::move(secret);
}

//===--------------------------------------------------------------------===//
// Extension load
//===--------------------------------------------------------------------===//

static void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("SQL table functions over the Live Tennis API: live matches, fixtures and player search");

	// Settings (fallback for environments where CREATE SECRET is not convenient)
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption("livetennis_api_key", "API key for the Live Tennis API (https://livetennisapi.com)",
	                          LogicalType::VARCHAR);
	config.AddExtensionOption("livetennis_base_url",
	                          "Override the Live Tennis API base URL (default " + string(DEFAULT_BASE_URL) + ")",
	                          LogicalType::VARCHAR);

	// Secret type: CREATE SECRET (TYPE livetennis, API_KEY '...')
	SecretType secret_type;
	secret_type.name = SECRET_TYPE;
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	loader.RegisterSecretType(secret_type);

	CreateSecretFunction secret_function;
	secret_function.secret_type = SECRET_TYPE;
	secret_function.provider = "config";
	secret_function.function = CreateLiveTennisSecret;
	secret_function.named_parameters["api_key"] = LogicalType::VARCHAR;
	secret_function.named_parameters["base_url"] = LogicalType::VARCHAR;
	loader.RegisterFunction(secret_function);

	// live_tennis_matches()
	TableFunction matches("live_tennis_matches", {}, LiveTennisScan, MatchesBind, MatchesInit);
	loader.RegisterFunction(matches);

	// live_tennis_fixtures() / live_tennis_fixtures(tour)
	TableFunctionSet fixtures("live_tennis_fixtures");
	fixtures.AddFunction(TableFunction({}, LiveTennisScan, FixturesBind, FixturesInit));
	fixtures.AddFunction(TableFunction({LogicalType::VARCHAR}, LiveTennisScan, FixturesBind, FixturesInit));
	loader.RegisterFunction(fixtures);

	// live_tennis_players(search)
	TableFunction players("live_tennis_players", {LogicalType::VARCHAR}, LiveTennisScan, PlayersBind, PlayersInit);
	loader.RegisterFunction(players);
}

void LivetennisExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string LivetennisExtension::Name() {
	return "livetennis";
}

std::string LivetennisExtension::Version() const {
#ifdef EXT_VERSION_LIVETENNIS
	return EXT_VERSION_LIVETENNIS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(livetennis, loader) {
	duckdb::LoadInternal(loader);
}
}
