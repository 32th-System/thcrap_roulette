/**
  * Touhou Community Reliant Automatic Patcher
  * Roulette
  *
  * ----
  *
  * Create a random thcrap configuration
  */


#include <thcrap.h>
#include <array>
#include <list>
#include <map>
#include <vector>
#include <string>
#include <string_view>
#include <thcrap_update_wrapper.h>

#include <win32_utf8/entry_main.c>

namespace crsh {
#include "exception.cpp"
}

// From select.cpp from thcrap_configure
typedef std::list<patch_desc_t> patch_sel_stack_t;

struct progress_state_t
{
	// This callback can be called from a bunch of threads
	std::mutex mutex;
	std::map<std::string, std::chrono::steady_clock::time_point> files;
};

char** games_json_to_array(json_t* games, const char* game)
{
	char** array;
	const char* key;
	json_t* value;

	array = strings_array_create();
	json_object_foreach(games, key, value) {
		if (game && *game) {
			if (strcmp(key, game) == 0) {
				array = strings_array_add(array, key);
			}
			continue;
		}
		array = strings_array_add(array, key);
	}
	return array;
}

bool progress_callback(progress_callback_status_t* status, void* param)
{
	using namespace std::literals;
	progress_state_t* state = (progress_state_t*)param;
	std::scoped_lock lock(state->mutex);

	switch (status->status) {
	case GET_DOWNLOADING: {
		// Using the URL instead of the filename is important, because we may
		// be downloading the same file from 2 different URLs, and the UI
		// could quickly become very confusing, with progress going backwards etc.
		auto& file_time = state->files[status->url];
		auto now = std::chrono::steady_clock::now();
		if (file_time.time_since_epoch() == 0ms) {
			file_time = now;
		}
		else if (now - file_time > 5s) {
			log_printf("[%u/%u] %s: in progress (%ub/%ub)...\n", status->nb_files_downloaded, status->nb_files_total,
				status->url, status->file_progress, status->file_size);
			file_time = now;
		}
		return true;
	}

	case GET_OK:
		log_printf("[%u/%u] %s/%s: OK (%ub)\n", status->nb_files_downloaded, status->nb_files_total, status->patch->id, status->fn, status->file_size);
		return true;

	case GET_CLIENT_ERROR:
	case GET_SERVER_ERROR:
	case GET_SYSTEM_ERROR:
		log_printf("%s: %s\n", status->url, status->error);
		return true;
	case GET_CRC32_ERROR:
		log_printf("%s: CRC32 error\n", status->url);
		return true;
	case GET_CANCELLED:
		// Another copy of the file have been downloader earlier. Ignore.
		return true;
	default:
		log_printf("%s: unknown status\n", status->url);
		return true;
	}
}

int file_write_text(const char* fn, const char* str)
{
	int ret;
	FILE* file = fopen_u(fn, "wt");
	if (!file) {
		return -1;
	}
	ret = fputs(str, file);
	fclose(file);
	return ret;
}

bool sel_match(const patch_desc_t& a, const patch_desc_t& b)
{
	bool absolute;
	bool repo_match;
	bool patch_match;

	absolute = a.repo_id && b.repo_id;

	if (absolute) {
		repo_match = (strcmp(a.repo_id, b.repo_id) == 0);
	}
	else {
		repo_match = true;
	}

	patch_match = (strcmp(a.patch_id, b.patch_id) == 0);

	return repo_match && patch_match;
}

repo_t* find_repo_in_list(repo_t** repo_list, const char* repo_id)
{
	for (size_t i = 0; repo_list[i]; i++) {
		if (strcmp(repo_list[i]->id, repo_id) == 0) {
			return repo_list[i];
		}
	}
	return nullptr;
}

const repo_patch_t* find_patch_in_repo(const repo_t* repo, const char* patch_id)
{
	for (size_t i = 0; repo && repo->patches[i].patch_id; i++) {
		if (strcmp(repo->patches[i].patch_id, patch_id) == 0) {
			return &repo->patches[i];
		}
	}
	return nullptr;
}

// Locates a repository for [sel] in [repo_list], starting from [orig_repo_id].
std::string SearchPatch(repo_t** repo_list, const char* orig_repo_id, const patch_desc_t& sel)
{
	const repo_t* orig_repo = find_repo_in_list(repo_list, orig_repo_id);

	// Absolute dependency
	// In fact, just a check to see whether [sel] is available.
	if (sel.repo_id) {
		repo_t* remote_repo = find_repo_in_list(repo_list, sel.repo_id);
		if (remote_repo) {
			const repo_patch_t* patch = find_patch_in_repo(remote_repo, sel.patch_id);
			if (patch) {
				return remote_repo->id;
			}
		}
		return "";
	}

	// Relative dependency
	if (find_patch_in_repo(orig_repo, sel.patch_id)) {
		return orig_repo_id;
	}

	for (size_t i = 0; repo_list[i]; i++) {
		if (find_patch_in_repo(repo_list[i], sel.patch_id)) {
			return repo_list[i]->id;
		}
	}

	// Not found...
	return "";
}

bool IsSelected(const patch_sel_stack_t& sel_stack, const patch_desc_t& sel)
{
	for (const patch_desc_t& it : sel_stack) {
		if (sel_match(sel, it)) {
			return true;
		}
	}
	return false;
}

int AddPatch(patch_sel_stack_t& sel_stack, repo_t** repo_list, patch_desc_t sel)
{
	int ret = 0;
	const repo_t* repo = find_repo_in_list(repo_list, sel.repo_id);
	patch_t patch_info = patch_bootstrap_wrapper(&sel, repo);
	patch_t patch_full = patch_init(patch_info.archive, nullptr, 0);
	patch_desc_t* dependencies = patch_full.dependencies;

	for (size_t i = 0; dependencies && dependencies[i].patch_id; i++) {
		patch_desc_t dep_sel = dependencies[i];

		if (!IsSelected(sel_stack, dep_sel)) {
			std::string target_repo = SearchPatch(repo_list, sel.repo_id, dep_sel);
			if (target_repo.empty()) {
				log_printf("ERROR: Dependency '%s/%s' of patch '%s' not met!\n", dep_sel.repo_id, dep_sel.patch_id, sel.patch_id);
				ret++;
			}
			else {
				free(dep_sel.repo_id);
				dep_sel.repo_id = strdup(target_repo.c_str());
				dependencies[i] = dep_sel;
				ret += AddPatch(sel_stack, repo_list, dep_sel);
			}
		}
	}

	stack_add_patch(&patch_full);
	sel_stack.push_back({ strdup(sel.repo_id), strdup(sel.patch_id) });
	patch_free(&patch_info);
	return ret;
}

void add_remove_vector_string(std::vector<std::string>& vec, std::string str) {
	bool removed = false;
	for (unsigned int i = 0; i < vec.size(); i++) {
		if (vec[i] == str) {
			vec.erase(vec.begin() + i);
			removed = true;
			break;
		}
	}
	if (!removed) {
		vec.push_back(str);
	}
}

bool vector_string_contains(std::vector<std::string>& vec, const char* str) {
	for (const std::string& _str : vec) {
		if (strcmp(_str.c_str(), str) == 0) {
			return true;
		}
	}
	return false;
}

const char* cmd_inp() {
	size_t size = 32;
	char* buf = (char*)malloc(size);
	size_t i = 0;
	char a;
	for (;;) {
		a = getchar();
		if (a == '\n') break;
		if (i == size) {
			size += 32;
			buf = (char*)realloc(buf, size);
		}
		buf[i++] = a;
	}
	buf[i] = 0;
	return buf;
}

void exclusion_input(std::vector<std::string>& exclude) {
	const char* inp = NULL;

exclusion_input_start:
	if (inp) free((void*)inp);
	printf("Currently excluded: ");
	for (const std::string& repo_name : exclude) {
		printf(repo_name.c_str());
		putchar(' ');
	}
	putchar('\n');
	puts("Press ENTER without typing anything to proceed");

	printf("\nEnter your input(s): ");
	inp = cmd_inp();
	putchar('\n'); putchar('\n');

	if (*inp) {
		const char* l = inp;

	search_inputs:
		while (*l == ' ')
			++l;

		if (*l == 0) {
			goto exclusion_input_start;
		}

		const char* l_ = strchr(l, ' ');
		if (l_) {
			add_remove_vector_string(exclude, std::string(l, l_ - l));
			l = l_;
			goto search_inputs;
		}
		else {
			add_remove_vector_string(exclude, l);
		}

		goto exclusion_input_start;
	}
	free((void*)inp);
}

bool yes_no(const char* q) {
	puts(q);
yesno_begin:

	puts("Y for yes, N for no");
	const char* a = cmd_inp();

	if (a[0] == 'Y' || a[0] == 'y') {
		free((void*)a);
		return true;
	}
	if (a[0] == 'N' || a[0] == 'n') {
		free((void*)a);
		return false;
	}
	free((void*)a);
	goto yesno_begin;
}

int TH_CDECL win32_utf8_main(int argc, const char** argv)
{
	AddVectoredExceptionHandler(0, crsh::exception_filter);
	VLA(char, current_dir, MAX_PATH);
	GetModuleFileNameU(NULL, current_dir, MAX_PATH);
	PathRemoveFileSpecU(current_dir);
	PathAppendU(current_dir, "..");
	SetCurrentDirectoryU(current_dir);
	VLA_FREE(current_dir);

	if (!thcrap_update_module()) {
		puts("thcrap_update" DEBUG_OR_RELEASE ".dll couldn't be loaded");
		return 1;
	}

	const char* start_url = "https://srv.thpatch.net/";

	// Copied from thcrap_wrapper/src/install_modules.c to fix linker error

	// From thcrap_update/src/http_status.h
	// Slightly modified since this is C
	typedef enum HttpStatus {
		// 200 - success
		HttpOk,
		// Download cancelled by the progress callback, or another client
		// declared the server as dead
		HttpCancelled,
		// 3XX and 4XX - file not found, not accessible, moved, etc.
		HttpClientError,
		// 5XX errors - server errors, further requests are likely to fail.
		// Also covers weird error codes like 1XX and 2XX which we shouldn't see.
		HttpServerError,
		// Error returned by the download library or by the write callback
		HttpSystemError,
		// Error encountered before loading thcrap_update.dll
		HttpLibLoadError
	} HttpStatus;

	typedef HttpStatus download_single_file_t(const char* url, const char* fn);
	HMODULE hUpdate = thcrap_update_module();
	download_single_file_t* download_single_file = (download_single_file_t*)GetProcAddress(hUpdate, "download_single_file");
	if (!download_single_file) {
		puts("FATAL ERROR: thcrap version too old!");
		getchar();
		return 1;
	}

	std::vector<std::string> repo_exclude;
	std::vector<std::string> patch_exclude;

	download_single_file("https://raw.githubusercontent.com/touhoureplayshowcase/thcrap_roulette/master/blacklist.json", "blacklist.json");
	if (!PathFileExistsW(L"blacklist.json")) {
		puts("Failed to download blacklist.json!");
		puts("Proceeding with no blacklist");
		goto after_blacklist;
	}
	json_t* blacklist = json_load_file("blacklist.json", 0, nullptr);
	if (!json_is_object(blacklist)) goto after_blacklist_init;

	json_t* repo_exclude_j = json_object_get(blacklist, "repo_exclude");
	if(json_is_array(repo_exclude_j)) {
		size_t i;
		json_t* val;
		json_array_foreach(repo_exclude_j, i, val) {
			if (const char* repo = json_string_value(val))
				repo_exclude.push_back(repo);
		}
	}

	json_t* patch_exclude_j = json_object_get(blacklist, "patch_exclude");
	if (json_is_array(patch_exclude_j)) {
		size_t i;
		json_t* val;
		json_array_foreach(patch_exclude_j, i, val) {
			if (const char* patch = json_string_value(val))
				patch_exclude.push_back(patch);
		}
	}

	after_blacklist_init:
	puts("Do you want exclude any patch repos from the roulette?");
	puts("If you type the name of a repo already in this list, it will be removed from the list");
	puts("You can also specify multiple repo names, separated by spaces");
	exclusion_input(repo_exclude);

	puts("Do you want exclude any patches from the roulette?");
	puts("If you type the name of a patch already in this list, it will be removed from the list");
	puts("You can also specify multiple patch names, separated by spaces");
	exclusion_input(patch_exclude);

	after_blacklist:
	patch_exclude.push_back("anm_leak");
	patch_exclude.push_back("debug_counters");

	puts("Which game do you want to patch?");
	puts("Press ENTER without typing anything to proceed");
	char game_inp[16] = {};
	fgets(game_inp, 16, stdin);
	*strchr(game_inp, '\n') = 0;

	if (strcmp(game_inp, "th18") == 0) patch_exclude.push_back("bullet-cap");

	puts("Downloading patchlist...");
	repo_t** repos = RepoDiscover_wrapper(start_url);

	std::vector<patch_desc_t> patches;

	for (int i = 0; repos[i] != NULL; ++i) {
		if (!vector_string_contains(repo_exclude, repos[i]->id)) {
			for (int j = 0; repos[i]->patches[j].patch_id != NULL; ++j) {
				if (!vector_string_contains(patch_exclude, repos[i]->patches[j].patch_id)) {
					if (*game_inp == 0) goto patches_push_back;

					json_t* files_js = NULL;
					for (int k = 0; repos[i]->servers[k]; ++k) {
						std::string _url = repos[i]->servers[k];
						_url += repos[i]->patches[j].patch_id;
						_url += "/files.js";
						HttpStatus status = download_single_file(_url.c_str(), "files.js");
						if (status == HttpOk) {
							files_js = json_load_file("files.js", 0, nullptr);
							if (files_js) break;
						}
					}
					if (!files_js) continue;
					const char* fn;
					json_t* crc;

					json_object_foreach(files_js, fn, crc) {
						if (strstr(fn, game_inp)) goto patches_push_back;
					}
					continue;
					patches_push_back:
					patches.push_back({ repos[i]->id, repos[i]->patches[j].patch_id });
				}
			}
		}
	}	

	char _num_patches[8];
	unsigned int num_patches;
sel_num_patches:
	printf("Number of patches (max: %d): ", patches.size());
	fgets(_num_patches, 8, stdin);
	num_patches = atoi(_num_patches);
	if (num_patches == 0 || num_patches > patches.size()) {
		puts("Try again");
		goto sel_num_patches;
	}
	printf("%d patches\n\n", num_patches);

	FILETIME _time;
	GetSystemTimeAsFileTime(&_time);
	srand(_time.dwLowDateTime);

	patch_sel_stack_t stack;

	for (unsigned int i = 0; i < num_patches; i++) {
		int idx = rand() % patches.size();
		patch_desc_t patch = patches[idx];
		patches.erase(patches.begin() + idx);
		AddPatch(stack, repos, patch);
	}
	
	if(yes_no("Do you want to add anm_leak, a patch that fixes crash and lag issues related to rendering?"))
		AddPatch(stack, repos, { "ExpHP", "anm_leak" });

	if (yes_no("Do you want to add debug_counters, a patch that will show various information about the game's state?"))
		AddPatch(stack, repos, { "ExpHP", "debug_counters" });

	/// Build the new run configuration
	json_t* new_cfg = json_pack("{s[]}", "patches");
	json_t* new_cfg_patches = json_object_get(new_cfg, "patches");
	for (patch_desc_t& sel : stack) {
		patch_t patch = patch_build(&sel);
		json_array_append_new(new_cfg_patches, patch_to_runconfig_json(&patch));
		patch_free(&patch);
	}
	json_object_set_new(new_cfg, "console", json_false());
	json_object_set_new(new_cfg, "dat_dump", json_false());
	if (*game_inp) {
		json_object_set_new(new_cfg, "game", json_string(game_inp));
	}

	const char* run_cfg_str = json_dumps(new_cfg, JSON_INDENT(2) | JSON_SORT_KEYS);
	file_write_text("config/random.js", run_cfg_str);
	puts("You rolled:");
	puts(run_cfg_str);
	puts("Saved to config/random.js. Press ENTER to start downloading");
	puts("NOTE: only data for games already in your games.js will be downloaded");
	getchar();

	log_init(1);
	progress_state_t state;
	stack_update_wrapper(update_filter_global_wrapper, NULL, progress_callback, &state);
	state.files.clear();

	json_t* games_js = json_load_file_report("config/games.js");
	char** filter = games_json_to_array(games_js, game_inp);

	stack_update_wrapper(update_filter_games_wrapper, filter, progress_callback, &state);
	state.files.clear();

	log_flush();
	puts("\n\nDone! You can now run roulette_launch.bat to lauch.\nPress ENTER to close");
	getchar();

	return 0;
}
