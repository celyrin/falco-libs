/*
Copyright (C) 2022 The Falco Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#include <stdio.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#endif // _WIN32

#include "scap_open_exception.h"
#include "sinsp.h"
#include "sinsp_int.h"
#include "sinsp_auth.h"
#include "filter.h"
#include "filterchecks.h"
#include "cyclewriter.h"
#include "protodecoder.h"
#include "dns_manager.h"
#include "plugin.h"
#include "plugin_manager.h"
#include "plugin_filtercheck.h"
#include "strl.h"

#if !defined(CYGWING_AGENT)
#if !defined(MINIMAL_BUILD) && !defined(__EMSCRIPTEN__)
#include "k8s_api_handler.h"
#if defined(HAS_CAPTURE)
#include <curl/curl.h>
#endif // defined(HAS_CAPTURE)
#else // !defined(MINIMAL_BUILD) && !defined(__EMSCRIPTEN__)
struct k8s_api_handler{}; // note: makes the unique_ptr static asserts happy
#endif // !defined(MINIMAL_BUILD) && !defined(__EMSCRIPTEN__)
#endif // !defined(CYGWING_AGENT)

#ifdef GATHER_INTERNAL_STATS
#include "stats.h"
#else // GATHER_INTERNAL_STATS
struct sinsp_stats{}; // note: makes the unique_ptr static asserts happy
#endif // GATHER_INTERNAL_STATS

#ifdef HAS_ANALYZER
#include "analyzer_int.h"
#include "analyzer.h"
#include "tracer_emitter.h"
#endif

void on_new_entry_from_proc(void* context, int64_t tid, scap_threadinfo* tinfo,
							scap_fdinfo* fdinfo);

///////////////////////////////////////////////////////////////////////////////
// sinsp implementation
///////////////////////////////////////////////////////////////////////////////
std::atomic<int> sinsp::instance_count{0};

sinsp::sinsp(bool static_container, const std::string &static_id, const std::string &static_name, const std::string &static_image) :
	m_external_event_processor(),
	m_evt(this),
	m_lastevent_ts(0),
	m_host_root(scap_get_host_root()),
	m_container_manager(this, static_container, static_id, static_name, static_image),
	m_usergroup_manager(this),
	m_k8s_api_handler(nullptr),
	m_k8s_ext_handler(nullptr),
	m_stats(nullptr),
	m_suppressed_comms(),
	m_inited(false)
{
	++instance_count;
#if !defined(MINIMAL_BUILD) && !defined(CYGWING_AGENT) && !defined(__EMSCRIPTEN__) && defined(HAS_CAPTURE) 
	// used by mesos and container_manager
	curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
	m_h = NULL;
	m_parser = NULL;
	m_is_dumping = false;
	m_metaevt = NULL;
	m_meinfo.m_piscapevt = NULL;
	m_parser = new sinsp_parser(this);
	m_thread_manager = new sinsp_thread_manager(this);
	m_max_fdtable_size = MAX_FD_TABLE_SIZE;
	m_inactive_container_scan_time_ns = DEFAULT_INACTIVE_CONTAINER_SCAN_TIME_S * ONE_SECOND_IN_NS;
	m_deleted_users_groups_scan_time_ns = DEFAULT_DELETED_USERS_GROUPS_SCAN_TIME_S * ONE_SECOND_IN_NS;
	m_cycle_writer = NULL;
	m_write_cycling = false;
	m_filter = NULL;
	m_fds_to_remove = new std::vector<int64_t>;
	m_machine_info = NULL;
	m_agent_info = NULL;
#ifdef SIMULATE_DROP_MODE
	m_isdropping = false;
#endif
	m_snaplen = DEFAULT_SNAPLEN;
	m_buffer_format = sinsp_evt::PF_NORMAL;
	m_input_fd = 0;
	m_isdebug_enabled = false;
	m_isfatfile_enabled = false;
	m_isinternal_events_enabled = false;
	m_hostname_and_port_resolution_enabled = false;
	m_output_time_flag = 'h';
	m_max_evt_output_len = 0;
	m_filesize = -1;
	m_track_tracers_state = false;
	m_next_flush_time_ns = 0;
	m_last_procrequest_tod = 0;
	m_get_procs_cpu_from_driver = false;
	m_is_tracers_capture_enabled = false;
	m_flush_memory_dump = false;
	m_next_stats_print_time_ns = 0;
	m_large_envs_enabled = false;
	m_increased_snaplen_port_range = DEFAULT_INCREASE_SNAPLEN_PORT_RANGE;
	m_statsd_port = -1;

	// Unless the cmd line arg "-pc" or "-pcontainer" is supplied this is false
	m_print_container_data = false;

#if defined(HAS_CAPTURE)
	m_self_pid = getpid();
#endif

#ifdef GATHER_INTERNAL_STATS
	m_stats = std::make_unique<sinsp_stats()>;
#endif

	m_proc_scan_timeout_ms = SCAP_PROC_SCAN_TIMEOUT_NONE;
	m_proc_scan_log_interval_ms = SCAP_PROC_SCAN_LOG_NONE;

	uint32_t evlen = sizeof(scap_evt) + 2 * sizeof(uint16_t) + 2 * sizeof(uint64_t);
	m_meinfo.m_piscapevt = (scap_evt*)new char[evlen];
	m_meinfo.m_piscapevt->type = PPME_PROCINFO_E;
	m_meinfo.m_piscapevt->len = evlen;
	m_meinfo.m_piscapevt->nparams = 2;
	uint16_t* lens = (uint16_t*)((char *)m_meinfo.m_piscapevt + sizeof(struct ppm_evt_hdr));
	lens[0] = 8;
	lens[1] = 8;
	m_meinfo.m_piscapevt_vals = (uint64_t*)(lens + 2);

	m_meinfo.m_pievt.m_inspector = this;
	m_meinfo.m_pievt.m_info = &(g_infotables.m_event_info[PPME_SCAPEVENT_X]);
	m_meinfo.m_pievt.m_pevt = NULL;
	m_meinfo.m_pievt.m_cpuid = 0;
	m_meinfo.m_pievt.m_evtnum = 0;
	m_meinfo.m_pievt.m_pevt = m_meinfo.m_piscapevt;
	m_meinfo.m_pievt.m_fdinfo = NULL;
	m_meinfo.m_n_procinfo_evts = 0;
	m_meta_event_callback = NULL;
	m_meta_event_callback_data = NULL;
#if !defined(CYGWING_AGENT) && !defined(MINIMAL_BUILD) && !defined(__EMSCRIPTEN__)
	m_k8s_client = NULL;
	m_k8s_last_watch_time_ns = 0;

	m_k8s_client = NULL;
	m_k8s_api_server = NULL;
	m_k8s_api_cert = NULL;

	m_mesos_client = NULL;
	m_mesos_last_watch_time_ns = 0;
#endif // !defined(CYGWING_AGENT) && !defined(MINIMAL_BUILD)

	m_replay_scap_evt = NULL;

	// the "syscall" event source is implemented by sinsp itself
	// and is always present
	m_plugin_parsers.clear();
	m_event_sources.push_back(sinsp_syscall_event_source_name);
	m_plugin_manager = std::make_shared<sinsp_plugin_manager>(m_event_sources);

	// create state tables registry
	m_table_registry = std::make_shared<libsinsp::state::table_registry>();
	m_table_registry->add_table(m_thread_manager);
}

sinsp::~sinsp()
{
	close();

	if(m_fds_to_remove)
	{
		delete m_fds_to_remove;
	}

	if(m_parser)
	{
		delete m_parser;
		m_parser = NULL;
	}

	if(m_thread_manager)
	{
		delete m_thread_manager;
		m_thread_manager = NULL;
	}

	if(m_cycle_writer)
	{
		delete m_cycle_writer;
		m_cycle_writer = NULL;
	}

	if(m_meinfo.m_piscapevt)
	{
		delete[] m_meinfo.m_piscapevt;
	}

	m_container_manager.cleanup();

#if !defined(CYGWING_AGENT) && !defined(MINIMAL_BUILD) && !defined(__EMSCRIPTEN__)
	delete m_k8s_client;
	delete m_k8s_api_server;
	delete m_k8s_api_cert;

	delete m_mesos_client;
#ifdef HAS_CAPTURE
	curl_global_cleanup();
	if (--instance_count == 0)
	{
		sinsp_dns_manager::get().cleanup();
	}
#endif
#endif
}

void sinsp::add_protodecoders()
{
	m_parser->add_protodecoder("syslog");
}

void sinsp::enable_tracers_capture()
{
#if defined(HAS_CAPTURE) && ! defined(CYGWING_AGENT) && ! defined(_WIN32)
	if(!m_is_tracers_capture_enabled)
	{
		if(is_live() && m_h != NULL)
		{
			if(scap_enable_tracers_capture(m_h) != SCAP_SUCCESS)
			{
				throw sinsp_exception("error enabling tracers capture");
			}
		}

		m_is_tracers_capture_enabled = true;
	}
#endif
}

bool sinsp::is_initialstate_event(scap_evt* pevent)
{
	return  pevent->type == PPME_CONTAINER_E ||
			pevent->type == PPME_CONTAINER_JSON_E ||
			pevent->type == PPME_CONTAINER_JSON_2_E ||
			pevent->type == PPME_USER_ADDED_E ||
			pevent->type == PPME_USER_DELETED_E ||
			pevent->type == PPME_GROUP_ADDED_E ||
			pevent->type == PPME_GROUP_DELETED_E;
}

void sinsp::consume_initialstate_events()
{
	scap_evt* pevent;
	uint16_t pcpuid;
	sinsp_evt* tevt;

	if (m_external_event_processor)
	{
		m_external_event_processor->on_capture_start();
	}

	//
	// Consume every state event we have
	//
	while(true)
	{
		int32_t res = scap_next(m_h, &pevent, &pcpuid);

		if(res == SCAP_SUCCESS)
		{
			// Setting these to non-null will make sinsp::next use them as a scap event
			// to avoid a call to scap_next. In this way, we can avoid the state parsing phase
			// once we reach a container-unrelated event.
			m_replay_scap_evt = pevent;
			m_replay_scap_cpuid = pcpuid;
			if(!is_initialstate_event(pevent))
			{
				break;
			}
			else
			{
				next(&tevt);
				continue;
			}
		}
		else
		{
			break;
		}
	}
}

void sinsp::init()
{
	//
	// Retrieve machine information
	//
	m_machine_info = scap_get_machine_info(m_h);
	if(m_machine_info != NULL)
	{
		m_num_cpus = m_machine_info->num_cpus;
	}
	else
	{
		ASSERT(false);
		m_num_cpus = 0;
	}

	//
	// Retrieve agent information
	//
	m_agent_info = scap_get_agent_info(m_h);
	if (m_agent_info == NULL)
	{
		ASSERT(false);
	}

	//
	// Attach the protocol decoders
	//
#ifndef HAS_ANALYZER
	add_protodecoders();
#endif
	//
	// Allocate the cycle writer
	//
	if(m_cycle_writer)
	{
		delete m_cycle_writer;
		m_cycle_writer = NULL;
	}

	m_cycle_writer = new cycle_writer(!is_offline());

	//
	// Basic inits
	//
#ifdef GATHER_INTERNAL_STATS
	m_stats->clear();
#endif

	m_nevts = 0;
	m_tid_to_remove = -1;
	m_lastevent_ts = 0;
	m_firstevent_ts = 0;
	m_fds_to_remove->clear();

	//
	// Return the tracers to the pool and clear the tracers list
	//
	for(auto it = m_partial_tracers_list.begin(); it != m_partial_tracers_list.end(); ++it)
	{
		m_partial_tracers_pool->push(*it);
	}
	m_partial_tracers_list.clear();

	//
	// If we're reading from file, we try to pre-parse the container events before
	// importing the thread table, so that thread table filtering will work with
	// container filters
	//
	if(is_capture())
	{
		consume_initialstate_events();
	}

	if(is_capture())
	{
		import_thread_table();
	}

	import_ifaddr_list();

	import_user_list();

	/* Create parent/child dependencies */
	m_thread_manager->create_thread_dependencies_after_proc_scan();

	//
	// Scan the list to fix the direction of the sockets
	//
	m_thread_manager->fix_sockets_coming_from_proc();

	// If we are in capture, this is already called by consume_initialstate_events
	if (!is_capture() && m_external_event_processor)
	{
		m_external_event_processor->on_capture_start();
	}

	//
	// If m_snaplen was modified, we set snaplen now
	//
	if(m_snaplen != DEFAULT_SNAPLEN)
	{
		set_snaplen(m_snaplen);
	}

	//
	// If the port range for increased snaplen was modified, set it now
	//
#ifndef _WIN32
	if(increased_snaplen_port_range_set())
	{
		set_fullcapture_port_range(m_increased_snaplen_port_range.range_start,
		                           m_increased_snaplen_port_range.range_end);
	}
#endif

	//
	// If the statsd port was modified, push it to the kernel now.
	//
	if(m_statsd_port != -1)
	{
		set_statsd_port(m_statsd_port);
	}

#if defined(HAS_CAPTURE)
	if(is_live())
	{
		int32_t res = scap_getpid_global(m_h, &m_self_pid);
		ASSERT(res == SCAP_SUCCESS || res == SCAP_NOT_SUPPORTED);
		(void)res;
	}
#endif
	m_inited = true;
}

void sinsp::set_import_users(bool import_users)
{
	m_usergroup_manager.m_import_users = import_users;
}

/*=============================== OPEN METHODS ===============================*/

void sinsp::open_common(scap_open_args* oargs)
{
	g_logger.log("Trying to open the right engine!");

	/* Reset the thread manager */
	m_thread_manager->clear();

	/* We need to save the actual mode and the engine used by the inspector. */
	m_mode = oargs->mode;

	if(oargs->mode != SCAP_MODE_CAPTURE)
	{
		oargs->proc_callback = ::on_new_entry_from_proc;
		oargs->proc_callback_context = this;
	}
	oargs->import_users = m_usergroup_manager.m_import_users;
	// We need to subscribe to container manager notifiers before
	// scap starts scanning proc.
	m_usergroup_manager.subscribe_container_mgr();

	add_suppressed_comms(oargs);

	oargs->debug_log_fn = &sinsp_scap_debug_log_fn;
	oargs->proc_scan_timeout_ms = m_proc_scan_timeout_ms;
	oargs->proc_scan_log_interval_ms = m_proc_scan_log_interval_ms;

	m_h = scap_alloc();
	if(m_h == NULL)
	{
		throw scap_open_exception("failed to allocate scap handle", SCAP_FAILURE);
	}

	int32_t scap_rc = scap_init(m_h, oargs);
	if(scap_rc != SCAP_SUCCESS)
	{
		std::string error = scap_getlasterr(m_h);
		scap_close(m_h);
		m_h = NULL;
		if(error.empty())
		{
			error = "Initialization issues during scap_init";
		}
		throw scap_open_exception(error, scap_rc);
	}

	init();

	// enable generation of async meta-events for all loaded plugins supporting
	// that capability. Meta-events are considered only during live captures,
	// because offline captures will have the async events already encoded
	// in the event stream.
	if (!is_capture())
	{
		// note(jasondellaluce,rohith-raju): for now the emscripten build does not support
		// tbb queues, so async event production is disabled
#ifndef __EMSCRIPTEN__
		for (auto& p : m_plugin_manager->plugins())
		{
			if (p->caps() & CAP_ASYNC)
			{
				auto res = p->set_async_event_handler([this](auto& p, auto e){
					this->handle_plugin_async_event(p, std::move(e));
				});
				if (!res)
				{
					throw sinsp_exception("can't set async event handler for plugin '"
						+ p->name() + "' : " + p->get_last_error());
				}
			}
		}
#endif
	}
}

scap_open_args sinsp::factory_open_args(const char* engine_name, scap_mode_t scap_mode)
{
	scap_open_args oargs{};
	oargs.engine_name = engine_name;
	oargs.mode = scap_mode;
	return oargs;
}

void sinsp::mark_ppm_sc_of_interest(ppm_sc_code ppm_sc, bool enable)
{
	/* This API must be used only after the initialization phase. */
	if (!m_inited)
	{
		throw sinsp_exception("you cannot use this method before opening the inspector!");
	}
	if (ppm_sc >= PPM_SC_MAX)
	{
		throw sinsp_exception("inexistent ppm_sc code: " + std::to_string(ppm_sc));
	}
	int ret = scap_set_ppm_sc(m_h, ppm_sc, enable);
	if (ret != SCAP_SUCCESS)
	{
		throw sinsp_exception(scap_getlasterr(m_h));
	}
}


static void fill_ppm_sc_of_interest(scap_open_args *oargs, const libsinsp::events::set<ppm_sc_code> &ppm_sc_of_interest)
{
	for (int i = 0; i < PPM_SC_MAX; i++)
	{
		/* If the set is empty, fallback to all interesting syscalls */
		if (ppm_sc_of_interest.empty())
		{
			oargs->ppm_sc_of_interest.ppm_sc[i] = true;
		}
		else
		{
			oargs->ppm_sc_of_interest.ppm_sc[i] = ppm_sc_of_interest.contains((ppm_sc_code)i);
		}
	}
}

void sinsp::open_kmod(unsigned long driver_buffer_bytes_dim, const libsinsp::events::set<ppm_sc_code> &ppm_sc_of_interest)
{
	scap_open_args oargs = factory_open_args(KMOD_ENGINE, SCAP_MODE_LIVE);

	/* Set interesting syscalls and tracepoints. */
	fill_ppm_sc_of_interest(&oargs, ppm_sc_of_interest);

	/* Engine-specific args. */
	struct scap_kmod_engine_params params;
	params.buffer_bytes_dim = driver_buffer_bytes_dim;
	oargs.engine_params = &params;
	open_common(&oargs);
}

void sinsp::open_bpf(const std::string& bpf_path, unsigned long driver_buffer_bytes_dim, const libsinsp::events::set<ppm_sc_code> &ppm_sc_of_interest)
{
	/* Validate the BPF path. */
	if(bpf_path.empty())
	{
		throw sinsp_exception("When you use the 'BPF' engine you need to provide a path to the bpf object file.");
	}

	scap_open_args oargs = factory_open_args(BPF_ENGINE, SCAP_MODE_LIVE);

	/* Set interesting syscalls and tracepoints. */
	fill_ppm_sc_of_interest(&oargs, ppm_sc_of_interest);

	/* Engine-specific args. */
	struct scap_bpf_engine_params params;
	params.buffer_bytes_dim = driver_buffer_bytes_dim;
	params.bpf_probe = bpf_path.data();
	oargs.engine_params = &params;
	open_common(&oargs);
}

void sinsp::open_udig()
{
	scap_open_args oargs = factory_open_args(UDIG_ENGINE, SCAP_MODE_LIVE);
	open_common(&oargs);
}

void sinsp::open_nodriver(bool full_proc_scan)
{
	scap_open_args oargs = factory_open_args(NODRIVER_ENGINE, SCAP_MODE_NODRIVER);
	struct scap_nodriver_engine_params params;
	params.full_proc_scan = full_proc_scan;

	oargs.engine_params = &params;
	open_common(&oargs);
}

void sinsp::open_savefile(const std::string& filename, int fd)
{
	scap_open_args oargs = factory_open_args(SAVEFILE_ENGINE, SCAP_MODE_CAPTURE);
	struct scap_savefile_engine_params params;

	m_input_filename = filename;
	m_input_fd = fd; /* default is 0. */

	if(m_input_fd != 0)
	{
		/* In this case, we can't get a reliable filesize */
		params.fd = m_input_fd;
		params.fname = NULL;
		m_filesize = 0;
	}
	else
	{
		if(filename.empty())
		{
			throw sinsp_exception("When you use the 'savefile' engine you need to provide a path to the file.");
		}

		params.fname = filename.c_str();
		params.fd = 0;

		char error[SCAP_LASTERR_SIZE] = {0};
		m_filesize = get_file_size(params.fname, error);
		if(m_filesize < 0)
		{
			throw sinsp_exception(error);
		}
	}

	params.start_offset = 0;
	params.fbuffer_size = 0;
	oargs.engine_params = &params;
	open_common(&oargs);
}

void sinsp::open_plugin(const std::string& plugin_name, const std::string& plugin_open_params, scap_mode_t mode)
{
	scap_open_args oargs = factory_open_args(SOURCE_PLUGIN_ENGINE, mode);
	struct scap_source_plugin_engine_params params;
	set_input_plugin(plugin_name, plugin_open_params);
	params.input_plugin = &m_input_plugin->as_scap_source();
	params.input_plugin_params = (char*)m_input_plugin_open_params.c_str();
	oargs.engine_params = &params;
	open_common(&oargs);
}

void sinsp::open_gvisor(const std::string& config_path, const std::string& root_path, bool no_events, int epoll_timeout)
{
	if(config_path.empty())
	{
		throw sinsp_exception("When you use the 'gvisor' engine you need to provide a path to the config file.");
	}

	scap_open_args oargs = factory_open_args(GVISOR_ENGINE, SCAP_MODE_LIVE);
	struct scap_gvisor_engine_params params;
	params.gvisor_root_path = root_path.c_str();
	params.gvisor_config_path = config_path.c_str();
	params.no_events = no_events;
	params.gvisor_epoll_timeout = epoll_timeout;
	oargs.engine_params = &params;
	open_common(&oargs);

	set_get_procs_cpu_from_driver(false);
}

void sinsp::open_modern_bpf(unsigned long driver_buffer_bytes_dim, uint16_t cpus_for_each_buffer, bool online_only, const libsinsp::events::set<ppm_sc_code> &ppm_sc_of_interest)
{
	scap_open_args oargs = factory_open_args(MODERN_BPF_ENGINE, SCAP_MODE_LIVE);

	/* Set interesting syscalls and tracepoints. */
	fill_ppm_sc_of_interest(&oargs, ppm_sc_of_interest);

	/* Engine-specific args. */
	struct scap_modern_bpf_engine_params params;
	params.buffer_bytes_dim = driver_buffer_bytes_dim;
	params.cpus_for_each_buffer = cpus_for_each_buffer;
	params.allocate_online_only = online_only;
	params.verbose = g_logger.has_output() && g_logger.is_enabled(sinsp_logger::severity::SEV_DEBUG);
	oargs.engine_params = &params;
	open_common(&oargs);
}

void sinsp::open_test_input(scap_test_input_data* data, scap_mode_t mode)
{
	scap_open_args oargs = factory_open_args(TEST_INPUT_ENGINE, mode);
	struct scap_test_input_engine_params params;
	params.test_input_data = data;
	oargs.engine_params = &params;
	open_common(&oargs);

	set_get_procs_cpu_from_driver(false);
}

/*=============================== OPEN METHODS ===============================*/

/*=============================== Engine related ===============================*/

bool sinsp::check_current_engine(const std::string& engine_name)
{
	return scap_check_current_engine(m_h, engine_name.data());
}

/*=============================== Engine related ===============================*/

std::string sinsp::generate_gvisor_config(std::string socket_path)
{
	return gvisor_config::generate(socket_path);
}

int64_t sinsp::get_file_size(const std::string& fname, char *error)
{
	static std::string err_str = "Could not determine capture file size: ";
	std::string errdesc;
#ifdef _WIN32
	LARGE_INTEGER li = { 0 };
	HANDLE fh = CreateFile(fname.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);
	if (fh != INVALID_HANDLE_VALUE)
	{
		if (0 != GetFileSizeEx(fh, &li))
		{
			CloseHandle(fh);
			return li.QuadPart;
		}
		errdesc = get_error_desc(err_str);
		CloseHandle(fh);
	}
#else
	struct stat st;
	if (0 == stat(fname.c_str(), &st))
	{
		return st.st_size;
	}
#endif
	if(errdesc.empty()) errdesc = get_error_desc(err_str);
	strlcpy(error, errdesc.c_str(), SCAP_LASTERR_SIZE);
	return -1;
}

unsigned sinsp::m_num_possible_cpus = 0;

unsigned sinsp::num_possible_cpus()
{
	if(m_num_possible_cpus == 0)
	{
		m_num_possible_cpus = read_num_possible_cpus();
		if(m_num_possible_cpus == 0)
		{
			g_logger.log("Unable to read num_possible_cpus, falling back to 128", sinsp_logger::SEV_WARNING);
			m_num_possible_cpus = 128;
		}
	}
	return m_num_possible_cpus;
}

std::vector<long> sinsp::get_n_tracepoint_hit()
{
	std::vector<long> ret(num_possible_cpus(), 0);
	if(scap_get_n_tracepoint_hit(m_h, ret.data()) != SCAP_SUCCESS)
	{
		throw sinsp_exception(scap_getlasterr(m_h));
	}
	return ret;
}

std::string sinsp::get_error_desc(const std::string& msg)
{
#ifdef _WIN32
	DWORD err_no = GetLastError(); // first, so error is not wiped out by intermediate calls
	std::string errstr = msg;
	DWORD flg = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
	LPTSTR msg_buf = 0;
	if(FormatMessageA(flg, 0, err_no, 0, (LPTSTR)&msg_buf, 0, NULL))
	if(msg_buf)
	{
		errstr.append(msg_buf, strlen(msg_buf));
		LocalFree(msg_buf);
	}
#else
	char* msg_buf = strerror(errno); // first, so error is not wiped out by intermediate calls
	std::string errstr = msg;
	if(msg_buf)
	{
		errstr.append(msg_buf, strlen(msg_buf));
	}
#endif
	return errstr;
}

void sinsp::close()
{
	if(m_h)
	{
		scap_close(m_h);
		m_h = NULL;
	}

	if(m_dumper != nullptr)
	{
		m_dumper->close();
		m_dumper.reset(nullptr);
	}

	m_is_dumping = false;

	deinit_state();

	if(m_filter != NULL)
	{
		delete m_filter;
		m_filter = NULL;
	}

	// unset the meta-event callback to all plugins that support it
	if (!is_capture() && m_mode != SCAP_MODE_NONE)
	{
		std::string err;
		// note(jasondellaluce,rohith-raju): for now the emscripten build does not support
		// tbb queues, so async event production is disabled
#ifndef __EMSCRIPTEN__
		for (auto& p : m_plugin_manager->plugins())
		{
			if (p->caps() & CAP_ASYNC)
			{
				// collect errors but let's make sure we reset all the handlers
				// event in case of one failure.
				auto res = p->set_async_event_handler(nullptr);
				if (!res)
				{
					err += err.empty() ? "" : ", ";
					err += "can't reset async event handler for plugin '"
						+ p->name() + "' : " + p->get_last_error();
				}
			}
		}
#endif
		if (!err.empty())
		{
			throw sinsp_exception(err);
		}
	}

	m_mode = SCAP_MODE_NONE;
}

//
// This deinitializes the sinsp internal state, and it's used
// internally while closing or restarting the capture.
//
void sinsp::deinit_state()
{
	m_network_interfaces.clear();
	m_thread_manager->clear();
}

void sinsp::autodump_start(const std::string& dump_filename, bool compress)
{
	if(NULL == m_h)
	{
		throw sinsp_exception("inspector not opened yet");
	}

	std::unique_ptr<sinsp_dumper> dumper(new sinsp_dumper);

	if(compress)
	{
		dumper->open(this, dump_filename.c_str(), SCAP_COMPRESSION_GZIP, false);
	}
	else
	{
		dumper->open(this, dump_filename.c_str(), SCAP_COMPRESSION_NONE, false);
	}

	m_is_dumping = true;

	m_dumper = std::move(dumper);

	m_container_manager.dump_containers(*m_dumper);

	m_usergroup_manager.dump_users_groups(*m_dumper);
}

void sinsp::autodump_next_file()
{
	autodump_stop();
	autodump_start(m_cycle_writer->get_current_file_name(), m_compress);
}

void sinsp::autodump_stop()
{
	if(NULL == m_h)
	{
		throw sinsp_exception("inspector not opened yet");
	}

	if(m_dumper != NULL)
	{
		m_dumper->close();
		m_dumper = NULL;
	}

	m_is_dumping = false;
}

void sinsp::on_new_entry_from_proc(void* context,
								   int64_t tid,
								   scap_threadinfo* tinfo,
								   scap_fdinfo* fdinfo)
{
	ASSERT(tinfo != NULL);

	//
	// Retrieve machine information if we don't have it yet
	//
	{
		m_machine_info = scap_get_machine_info(m_h);
		if(m_machine_info != NULL)
		{
			m_num_cpus = m_machine_info->num_cpus;
		}
		else
		{
			ASSERT(false);
			m_num_cpus = 0;
		}
	}

	//
	// Add the thread or FD
	//
	if(fdinfo == NULL)
	{
		bool thread_added = false;
		sinsp_threadinfo* newti = build_threadinfo();
		newti->init(tinfo);
		if(is_nodriver())
		{
			auto sinsp_tinfo = find_thread(tid, true);
			if(sinsp_tinfo == nullptr || newti->m_clone_ts > sinsp_tinfo->m_clone_ts)
			{
				thread_added = m_thread_manager->add_thread(newti, true);
			}
		}
		else
		{
			thread_added = m_thread_manager->add_thread(newti, true);
		}
		if (!thread_added) {
			delete newti;
		}
	}
	else
	{
		auto sinsp_tinfo = find_thread(tid, true);

		if(!sinsp_tinfo)
		{
			sinsp_threadinfo* newti = build_threadinfo();
			newti->init(tinfo);

			if (!m_thread_manager->add_thread(newti, true)) {
				ASSERT(false);
				delete newti;
				return;
			}

			sinsp_tinfo = find_thread(tid, true);
			if (!sinsp_tinfo) {
				ASSERT(false);
				return;
			}
		}

		sinsp_fdinfo_t sinsp_fdinfo;
		sinsp_tinfo->add_fd_from_scap(fdinfo, &sinsp_fdinfo);
	}
}

void on_new_entry_from_proc(void* context,
							int64_t tid,
							scap_threadinfo* tinfo,
							scap_fdinfo* fdinfo)
{
	sinsp* _this = (sinsp*)context;
	_this->on_new_entry_from_proc(context, tid, tinfo, fdinfo);
}

void sinsp::import_thread_table()
{
	scap_threadinfo *pi;
	scap_threadinfo *tpi;

	scap_threadinfo *table = scap_get_proc_table(m_h);

	//
	// Scan the scap table and add the threads to our list
	//
	HASH_ITER(hh, table, pi, tpi)
	{
		sinsp_threadinfo* newti = build_threadinfo();
		newti->init(pi);
		m_thread_manager->add_thread(newti, true);
	}
}

void sinsp::import_ifaddr_list()
{
	m_network_interfaces.clear();
	m_network_interfaces.import_interfaces(scap_get_ifaddr_list(m_h));
}

const sinsp_network_interfaces& sinsp::get_ifaddr_list()
{
	return m_network_interfaces;
}

void sinsp::import_ipv4_interface(const sinsp_ipv4_ifinfo& ifinfo)
{
	m_network_interfaces.import_ipv4_interface(ifinfo);
}

void sinsp::import_user_list()
{
	uint32_t j;
	scap_userlist* ul = scap_get_user_list(m_h);

	if(ul)
	{
		for(j = 0; j < ul->nusers; j++)
		{
			m_usergroup_manager.add_user("", -1, ul->users[j].uid, ul->users[j].gid, ul->users[j].name, ul->users[j].homedir, ul->users[j].shell);
		}

		for(j = 0; j < ul->ngroups; j++)
		{
			m_usergroup_manager.add_group("", -1, ul->groups[j].gid, ul->groups[j].name);
		}
	}
}

void sinsp::refresh_ifaddr_list()
{
#if defined(HAS_CAPTURE) && !defined(_WIN32)
	if(is_live() || is_syscall_plugin())
	{
		scap_refresh_iflist(m_h);
		m_network_interfaces.clear();
		m_network_interfaces.import_interfaces(scap_get_ifaddr_list(m_h));
	}
#endif
}

bool should_drop(sinsp_evt *evt, bool* stopped, bool* switched);

void sinsp::add_meta_event(sinsp_evt *metaevt)
{
	m_metaevt = metaevt;
}

void sinsp::add_meta_event_callback(meta_event_callback cback, void* data)
{
	m_meta_event_callback = cback;
	m_meta_event_callback_data = data;
}

void sinsp::remove_meta_event_callback()
{
	m_meta_event_callback = NULL;
}

void schedule_next_threadinfo_evt(sinsp* _this, void* data)
{
	sinsp_proc_metainfo* mei = (sinsp_proc_metainfo*)data;
	ASSERT(mei->m_pli != NULL);

	while(true)
	{
		ASSERT(mei->m_cur_procinfo_evt <= (int32_t)mei->m_n_procinfo_evts);
		ppm_proc_info* pi = &(mei->m_pli->entries[mei->m_cur_procinfo_evt]);

		if(mei->m_cur_procinfo_evt >= 0)
		{
			mei->m_piscapevt->tid = pi->pid;
			mei->m_piscapevt_vals[0] = pi->utime;
			mei->m_piscapevt_vals[1] = pi->stime;
		}

		mei->m_cur_procinfo_evt++;

		if(mei->m_cur_procinfo_evt < (int32_t)mei->m_n_procinfo_evts)
		{
			if(pi->utime == 0 && pi->stime == 0)
			{
				continue;
			}

			_this->add_meta_event(&mei->m_pievt);
		}

		break;
	}
}

//
// This restarts the current event capture. This de-initializes and
// re-initializes the internal state of both sinsp and scap, and is
// supported only for opened captures with mode SCAP_MODE_CAPTURE.
// This resets the internal states on-the-fly, which is ideally equivalent
// to closing and then re-opening the capture, but avoids losing the passed
// configurations and reuses the same underlying scap event source.
//
void sinsp::restart_capture()
{
	// Save state info that could be lost during de-initialization
	uint64_t nevts = m_nevts;

	// De-initialize the insternal state
	deinit_state();

	// Restart the scap capture, which also trigger a re-initialization of
	// scap's internal state.
	if (scap_restart_capture(m_h) != SCAP_SUCCESS)
	{
		throw sinsp_exception(std::string("scap error: ") + scap_getlasterr(m_h));
	}

	// Re-initialize the internal state
	init();

	// Restore the saved state info
	m_nevts = nevts;
}

uint64_t sinsp::max_buf_used()
{
	if(m_h)
	{
		return scap_max_buf_used(m_h);
	}
	else
	{
		return 0;
	}
}

void sinsp::get_procs_cpu_from_driver(uint64_t ts)
{
	if(ts <= m_next_flush_time_ns)
	{
		return;
	}

	uint64_t next_full_second = ts - (ts % ONE_SECOND_IN_NS) + ONE_SECOND_IN_NS;

	if(m_next_flush_time_ns == 0)
	{
		m_next_flush_time_ns = next_full_second;
		return;
	}

	m_next_flush_time_ns = next_full_second;

	uint64_t procrequest_tod = sinsp_utils::get_current_time_ns();
	if(procrequest_tod - m_last_procrequest_tod <= ONE_SECOND_IN_NS / 2)
	{
		return;
	}

	m_last_procrequest_tod = procrequest_tod;

	m_meinfo.m_pli = scap_get_threadlist(m_h);
	if(m_meinfo.m_pli == NULL)
	{
		throw sinsp_exception(std::string("scap error: ") + scap_getlasterr(m_h));
	}

	m_meinfo.m_n_procinfo_evts = m_meinfo.m_pli->n_entries;
	if(m_meinfo.m_n_procinfo_evts > 0)
	{
		m_meinfo.m_cur_procinfo_evt = -1;

		m_meinfo.m_piscapevt->ts = ts;
		add_meta_event_callback(&schedule_next_threadinfo_evt, &m_meinfo);
		schedule_next_threadinfo_evt(this, &m_meinfo);
	}
}

int32_t sinsp::next(OUT sinsp_evt **puevt)
{
	sinsp_evt* evt;
	int32_t res;

	//
	// Check if there are fake cpu events to  events
	//
	if(m_metaevt != NULL)
	{
		res = SCAP_SUCCESS;
		evt = m_metaevt;
		m_metaevt = NULL;

		if(m_meta_event_callback != NULL)
		{
			m_meta_event_callback(this, m_meta_event_callback_data);
		}
	}
#ifndef __EMSCRIPTEN__	
	else if (m_pending_state_evts.try_pop(m_state_evt))
	{
		res = SCAP_SUCCESS;
		evt = m_state_evt.get();
		// note: this is just a convention. When the timestamp is
		//       assigned to (uint64_t)-1 libsinsp is allowed to
		//       change that value, otherwise will keep the one
		//       previously assigned.
		if(evt->m_pevt->ts == (uint64_t) - 1)
		{
			evt->m_pevt->ts = get_new_ts();
		}
	}
#endif
	else
	{
		evt = &m_evt;

		//
		// Reset previous event's decoders if required
		//
		if(m_decoders_reset_list.size() != 0)
		{
			std::vector<sinsp_protodecoder*>::iterator it;
			for(it = m_decoders_reset_list.begin(); it != m_decoders_reset_list.end(); ++it)
			{
				(*it)->on_reset(evt);
			}

			m_decoders_reset_list.clear();
		}

		//
		// Get the event from libscap
		//
		if (m_replay_scap_evt != NULL)
		{
			// Replay the last event, if we saved one
			res = SCAP_SUCCESS;
			evt->m_pevt = m_replay_scap_evt;
			evt->m_cpuid = m_replay_scap_cpuid;
			m_replay_scap_evt = NULL;
		}
		else 
		{
			// If no last event was saved, invoke
			// the actual scap_next
			res = scap_next(m_h, &(evt->m_pevt), &(evt->m_cpuid));
		}

		if(res != SCAP_SUCCESS)
		{
			if(res == SCAP_TIMEOUT)
			{
				if (m_external_event_processor)
				{
					m_external_event_processor->process_event(NULL, libsinsp::EVENT_RETURN_TIMEOUT);
				}
				*puevt = NULL;
				return res;
			}
			else if(res == SCAP_EOF)
			{
				if (m_external_event_processor)
				{
					m_external_event_processor->process_event(NULL, libsinsp::EVENT_RETURN_EOF);
				}
			}
			else if(res == SCAP_UNEXPECTED_BLOCK)
			{
				// This mostly happens in concatenated scap files, where an unexpected block
				// represents the end of a file and the start of the next appended one.
				// In this case, we restart the capture so that the internal states gets reset
				// and the blocks coming from the next appended file get consumed.
				restart_capture();
				return SCAP_TIMEOUT;

			}
			else if(res == SCAP_FILTERED_EVENT)
			{
				// This will happen if SCAP has filtered the event in userspace (tid suppression).
				// A valid event was read from the driver, but we are choosing to not report it to
				// the client at the client's request.
				// However, we still need to return here so that the client doesn't time out the
				// request.
				if(m_external_event_processor)
				{
					m_external_event_processor->process_event(NULL, libsinsp::EVENT_RETURN_FILTERED);
					*puevt = NULL;
					return res;
				}
			}
			else
			{
				m_lasterr = scap_getlasterr(m_h);
			}

			return res;
		}
	}

	/* Here we shouldn't receive unknown events */
	ASSERT(!libsinsp::events::is_unknown_event((ppm_event_code)evt->get_type()));

	uint64_t ts = evt->get_ts();

	if(m_firstevent_ts == 0 &&
		!libsinsp::events::is_metaevent((ppm_event_code) evt->get_type()))
	{
		m_firstevent_ts = ts;
	}

	//
	// If required, retrieve the processes cpu from the kernel
	//
	if(m_get_procs_cpu_from_driver && is_live())
	{
		get_procs_cpu_from_driver(ts);
	}

	//
	// Store a couple of values that we'll need later inside the event.
	// These are potentially used both for parsing the event for internal
	// state management, and for managing the k8s and mesos clients.
	//
	m_nevts++;
	evt->m_evtnum = m_nevts;
	m_lastevent_ts = ts;

	if(m_automatic_threadtable_purging)
	{
		//
		// Delayed removal of threads from the thread table, so that
		// things like exit() or close() can be parsed.
		//
		if(m_tid_to_remove != -1)
		{
			remove_thread(m_tid_to_remove);
			m_tid_to_remove = -1;
		}

		if(!is_offline())
		{
			m_thread_manager->remove_inactive_threads();
		}
	}

#ifndef HAS_ANALYZER

	if(is_debug_enabled() && is_live())
	{
		if(ts > m_next_stats_print_time_ns)
		{
			if(m_next_stats_print_time_ns)
			{
				print_capture_stats(sinsp_logger::SEV_DEBUG);
			}

			m_next_stats_print_time_ns = ts - (ts % ONE_SECOND_IN_NS) + ONE_SECOND_IN_NS;
		}
	}

	//
	// Run the periodic connection, thread and users/groups table cleanup
	//
	if(!is_offline())
	{
		m_container_manager.remove_inactive_containers();

#if !defined(CYGWING_AGENT) && !defined(MINIMAL_BUILD) && !defined(__EMSCRIPTEN__)
		update_k8s_state();

		if(m_mesos_client)
		{
			update_mesos_state();
		}

		m_usergroup_manager.clear_host_users_groups();
#endif // !defined(CYGWING_AGENT) && !defined(MINIMAL_BUILD)
	}
#endif // HAS_ANALYZER

	//
	// Delayed removal of the fd, so that
	// things like exit() or close() can be parsed.
	//
	uint32_t nfdr = (uint32_t)m_fds_to_remove->size();

	if(nfdr != 0)
	{
		/* This is a removal logic we shouldn't scan /proc. If we don't have the thread 
		 * to remove we are fine.
		 */
		sinsp_threadinfo* ptinfo = get_thread_ref(m_tid_of_fd_to_remove, false).get();
		if(!ptinfo)
		{
			ASSERT(false);
			return res;
		}

		for(uint32_t j = 0; j < nfdr; j++)
		{
			ptinfo->remove_fd(m_fds_to_remove->at(j));
		}

		m_fds_to_remove->clear();
	}

#ifdef SIMULATE_DROP_MODE
	bool sd = false;
	bool sw = false;

	if(m_analyzer)
	{
		m_analyzer->m_configuration->set_analyzer_sample_len_ns(500000000);
	}

	sd = should_drop(evt, &m_isdropping, &sw);
#endif

	//
	// Run the state engine
	//
#ifdef SIMULATE_DROP_MODE
	if(!sd || m_isdropping)
	{
		m_parser->process_event(evt);
	}

	if(sd && !m_isdropping)
	{
		*evt = NULL;
		return SCAP_TIMEOUT;
	}
#else
	m_parser->process_event(evt);
#endif

	// run plugin-implemented parsers
	// note: we run the parsers even if the event has been filtered out,
	// because we have no guarantee that the plugin parsers will not use a given
	// event for state updates. Sinsp understands this through the
	// EF_MODIFIES_STATE flag, which however is only relevant in the context of
	// the internal implementation of libsinsp.
	for (auto& pp : m_plugin_parsers)
	{
		// todo(jason): should we log parsing errors here?
		pp.process_event(evt, m_event_sources);
	}

	//
	// If needed, dump the event to file
	//
	if(NULL != m_dumper)
	{
		if(m_write_cycling)
		{
			switch(m_cycle_writer->consider(evt, m_dumper->written_bytes()))
			{
				case cycle_writer::NEWFILE:
					autodump_next_file();
					break;

				case cycle_writer::DOQUIT:
					stop_capture();
					return SCAP_EOF;
					break;

				case cycle_writer::SAMEFILE:
					// do nothing.
					break;
			}
		}

		m_dumper->dump(evt);
	}

	if(evt->m_filtered_out)
	{
		ppm_event_category cat = evt->get_category();

		// Skip the event, unless we're in internal events
		// mode and the category of this event is internal.
		if(!(m_isinternal_events_enabled && (cat & EC_INTERNAL)))
		{
			*puevt = evt;
			return SCAP_FILTERED_EVENT;
		}
	}

	//
	// Run the analysis engine
	//
	if (m_external_event_processor)
	{
		m_external_event_processor->process_event(evt, libsinsp::EVENT_RETURN_NONE);
	}

	// Clean parse related event data after analyzer did its parsing too
	m_parser->event_cleanup(evt);

	//
	// Update the last event time for this thread
	//
	if(evt->m_tinfo &&
		evt->get_type() != PPME_SCHEDSWITCH_1_E &&
		evt->get_type() != PPME_SCHEDSWITCH_6_E)
	{
		evt->m_tinfo->m_prevevent_ts = evt->m_tinfo->m_lastevent_ts;
		evt->m_tinfo->m_lastevent_ts = m_lastevent_ts;
	}

	//
	// Done
	//
	*puevt = evt;
	return res;
}

uint64_t sinsp::get_num_events()
{
	if(m_h)
	{
		return scap_event_get_num(m_h);
	}
	else
	{
		return 0;
	}
}

sinsp_threadinfo* sinsp::find_thread_test(int64_t tid, bool lookup_only)
{
	// TODO: we pay the refcount manipulation price here
	return &*find_thread(tid, lookup_only);
}

threadinfo_map_t::ptr_t sinsp::get_thread_ref(int64_t tid, bool query_os_if_not_found, bool lookup_only, bool main_thread)
{
	return m_thread_manager->get_thread_ref(tid, query_os_if_not_found, lookup_only, main_thread);
}

bool sinsp::add_thread(const sinsp_threadinfo *ptinfo)
{
	return m_thread_manager->add_thread((sinsp_threadinfo*)ptinfo, false);
}

void sinsp::remove_thread(int64_t tid)
{
	m_thread_manager->remove_thread(tid);
}

bool sinsp::suppress_events_comm(const std::string &comm)
{
	if(m_suppressed_comms.size() >= SCAP_MAX_SUPPRESSED_COMMS)
	{
		return false;
	}

	m_suppressed_comms.insert(comm);

	if(m_h)
	{
		if (scap_suppress_events_comm(m_h, comm.c_str()) != SCAP_SUCCESS)
		{
			return false;
		}
	}

	return true;
}

bool sinsp::suppress_events_tid(int64_t tid)
{
	if(m_h && scap_suppress_events_tid(m_h, tid) == SCAP_SUCCESS)
	{
		return true;
	}

	return false;
}

bool sinsp::check_suppressed(int64_t tid)
{
	return scap_check_suppressed_tid(m_h, tid);
}

void sinsp::add_suppressed_comms(scap_open_args* oargs)
{
	uint32_t i = 0;

	// Note--using direct pointers to values in
	// m_suppressed_comms. This is ok given that a scap_open()
	// will immediately follow after which the args won't be used.
	for(auto &comm : m_suppressed_comms)
	{
		oargs->suppressed_comms[i++] = comm.c_str();
	}

	oargs->suppressed_comms[i++] = NULL;
}

void sinsp::set_docker_socket_path(std::string socket_path)
{
	m_container_manager.set_docker_socket_path(std::move(socket_path));
}

void sinsp::set_query_docker_image_info(bool query_image_info)
{
	m_container_manager.set_query_docker_image_info(query_image_info);
}

void sinsp::set_cri_extra_queries(bool extra_queries)
{
	m_container_manager.set_cri_extra_queries(extra_queries);
}

void sinsp::set_cri_socket_path(const std::string& path)
{
	m_container_manager.set_cri_socket_path(path);
}

void sinsp::add_cri_socket_path(const std::string& path)
{
	m_container_manager.add_cri_socket_path(path);
}

void sinsp::set_cri_timeout(int64_t timeout_ms)
{
	m_container_manager.set_cri_timeout(timeout_ms);
}

void sinsp::set_cri_async(bool async)
{
	m_container_manager.set_cri_async(async);
}

void sinsp::set_container_labels_max_len(uint32_t max_label_len)
{
	m_container_manager.set_container_labels_max_len(max_label_len);
}

void sinsp::set_snaplen(uint32_t snaplen)
{
	//
	// If set_snaplen is called before opening of the inspector,
	// we register the value to be set after its initialization.
	//
	if(m_h == NULL)
	{
		m_snaplen = snaplen;
		return;
	}

	if(is_live() && scap_set_snaplen(m_h, snaplen) != SCAP_SUCCESS)
	{
		throw sinsp_exception(scap_getlasterr(m_h));
	}
}

void sinsp::set_dropfailed(bool dropfailed)
{
	if(is_live() && scap_set_dropfailed(m_h, dropfailed) != SCAP_SUCCESS)
	{
		throw sinsp_exception(scap_getlasterr(m_h));
	}
}

void sinsp::set_fullcapture_port_range(uint16_t range_start, uint16_t range_end)
{
	//
	// If set_fullcapture_port_range is called before opening of the inspector,
	// we register the value to be set after its initialization.
	//
	if(m_h == NULL)
	{
		m_increased_snaplen_port_range = {range_start, range_end};
		return;
	}

	if(!is_live())
	{
		throw sinsp_exception("set_fullcapture_port_range called on a trace file, plugin, or test engine");
	}

	if(scap_set_fullcapture_port_range(m_h, range_start, range_end) != SCAP_SUCCESS)
	{
		throw sinsp_exception(scap_getlasterr(m_h));
	}
}

void sinsp::set_statsd_port(const uint16_t port)
{
	//
	// If this method is called before opening of the inspector,
	// we register the value to be set after its initialization.
	//
	if(m_h == NULL)
	{
		m_statsd_port = port;
		return;
	}

	if(!is_live())
	{
		throw sinsp_exception("set_statsd_port called on a trace file, plugin, or test engine");
	}

	if(scap_set_statsd_port(m_h, port) != SCAP_SUCCESS)
	{
		throw sinsp_exception(scap_getlasterr(m_h));
	}
}

std::shared_ptr<sinsp_plugin> sinsp::register_plugin(const std::string& filepath)
{
	std::string errstr;
	std::shared_ptr<sinsp_plugin> plugin = sinsp_plugin::create(filepath, m_table_registry, errstr);
	if (!plugin)
	{
		throw sinsp_exception("cannot load plugin " + filepath + ": " + errstr.c_str());
	}

	try
	{
		m_plugin_manager->add(plugin);
		if (plugin->caps() & CAP_PARSING)
		{
			m_plugin_parsers.push_back(sinsp_plugin_parser(plugin));
		}
	}
	catch(sinsp_exception const& e)
	{
		throw sinsp_exception("cannot register plugin " + filepath + " in inspector: " + e.what());
	}

	return plugin;
}

std::shared_ptr<sinsp_plugin> sinsp::register_plugin(const plugin_api* api)
{
	std::string errstr;
	std::shared_ptr<sinsp_plugin> plugin = sinsp_plugin::create(api, m_table_registry, errstr);
	if (!plugin)
	{
		throw sinsp_exception("cannot load plugin with custom vtable: " + errstr);
	}

	try
	{
		m_plugin_manager->add(plugin);
		if (plugin->caps() & CAP_PARSING)
		{
			m_plugin_parsers.push_back(sinsp_plugin_parser(plugin));
		}
	}
	catch(sinsp_exception const& e)
	{
		throw sinsp_exception("cannot register plugin with custom vtable in inspector: " + std::string(e.what()));
	}

	return plugin;
}

void sinsp::set_input_plugin(const std::string& name, const std::string& params)
{
	for(auto& it : m_plugin_manager->plugins())
	{
		if(it->name() == name)
		{
			if(!(it->caps() & CAP_SOURCING))
			{
				throw sinsp_exception("plugin " + name + " has not event sourcing capabilities and cannot be used as input.");
			}
			m_input_plugin = it;
			m_input_plugin_open_params = params;
			return;
		}
	}
	throw sinsp_exception("plugin " + name + " does not exist");
}

void sinsp::stop_capture()
{
	if(scap_stop_capture(m_h) != SCAP_SUCCESS)
	{
		throw sinsp_exception(scap_getlasterr(m_h));
	}

	/* Print scap stats */
	print_capture_stats(sinsp_logger::SEV_DEBUG);

	/* Print the number of threads and fds in our tables */
	uint64_t thread_cnt = 0;
	uint64_t fd_cnt = 0;
	m_thread_manager->m_threadtable.loop([&thread_cnt, &fd_cnt] (sinsp_threadinfo& tinfo) {
		thread_cnt++;

		/* Only main threads have an associated fdtable */
		if(tinfo.is_main_thread())
		{
			auto fdtable_ptr = tinfo.get_fd_table();
			if(fdtable_ptr != nullptr)
			{
				fd_cnt += fdtable_ptr->size();
			}
		}
		return true;
	});
	g_logger.format(sinsp_logger::SEV_DEBUG,
		"total threads in the table:%" PRIu64
		", total fds in all threads:%" PRIu64
		"\n",
		thread_cnt,
		fd_cnt); 
}

void sinsp::start_capture()
{
	if(scap_start_capture(m_h) != SCAP_SUCCESS)
	{
		throw sinsp_exception(scap_getlasterr(m_h));
	}
}

#ifndef _WIN32
void sinsp::stop_dropping_mode()
{
	if(is_live())
	{
		g_logger.format(sinsp_logger::SEV_INFO, "stopping drop mode");

		if(scap_stop_dropping_mode(m_h) != SCAP_SUCCESS)
		{
			throw sinsp_exception(scap_getlasterr(m_h));
		}
	}
}

void sinsp::start_dropping_mode(uint32_t sampling_ratio)
{
	if(is_live())
	{
		g_logger.format(sinsp_logger::SEV_INFO, "setting drop mode to %" PRIu32, sampling_ratio);

		if(scap_start_dropping_mode(m_h, sampling_ratio) != SCAP_SUCCESS)
		{
			throw sinsp_exception(scap_getlasterr(m_h));
		}
	}
}
#endif // _WIN32

void sinsp::set_filter(sinsp_filter* filter)
{
	if(m_filter != NULL)
	{
		ASSERT(false);
		throw sinsp_exception("filter can only be set once");
	}

	m_filter = filter;
}

void sinsp::set_filter(const std::string& filter)
{
	if(m_filter != NULL)
	{
		ASSERT(false);
		throw sinsp_exception("filter can only be set once");
	}

	sinsp_filter_compiler compiler(this, filter);
	m_filter = compiler.compile();
	m_filterstring = filter;
	m_internal_flt_ast = compiler.get_filter_ast();
}

const std::string sinsp::get_filter()
{
	return m_filterstring;
}

std::shared_ptr<libsinsp::filter::ast::expr> sinsp::get_filter_ast()
{
	return m_internal_flt_ast;
}

bool sinsp::run_filters_on_evt(sinsp_evt *evt)
{
	//
	// First run the global filter, if there is one.
	//
	if(m_filter && m_filter->run(evt) == true)
	{
		return true;
	}

	return false;
}

const scap_machine_info* sinsp::get_machine_info()
{
	return m_machine_info;
}

const scap_agent_info* sinsp::get_agent_info()
{
	return m_agent_info;
}

scap_stats_v2* sinsp::get_sinsp_stats_v2_buffer()
{
	return m_sinsp_stats_v2;
}

void sinsp::get_filtercheck_fields_info(OUT std::vector<const filter_check_info*>& list)
{
	sinsp_utils::get_filtercheck_fields_info(list);
}

sinsp_filter_check* sinsp::new_generic_filtercheck()
{
	return new sinsp_filter_check_gen_event();
}

void sinsp::get_capture_stats(scap_stats* stats) const
{
	/* On purpose ignoring failures to not interrupt in case of stats retrieval failure. */
	scap_get_stats(m_h, stats);
}

void sinsp::print_capture_stats(sinsp_logger::severity sev) const
{
	scap_stats stats;
	get_capture_stats(&stats);

	g_logger.format(sev,
		"\nn_evts:%" PRIu64
		"\nn_drops:%" PRIu64
		"\nn_drops_buffer:%" PRIu64
		"\nn_drops_buffer_clone_fork_enter:%" PRIu64
		"\nn_drops_buffer_clone_fork_exit:%" PRIu64
		"\nn_drops_buffer_execve_enter:%" PRIu64
		"\nn_drops_buffer_execve_exit:%" PRIu64
		"\nn_drops_buffer_connect_enter:%" PRIu64
		"\nn_drops_buffer_connect_exit:%" PRIu64
		"\nn_drops_buffer_open_enter:%" PRIu64
		"\nn_drops_buffer_open_exit:%" PRIu64
		"\nn_drops_buffer_dir_file_enter:%" PRIu64
		"\nn_drops_buffer_dir_file_exit:%" PRIu64
		"\nn_drops_buffer_other_interest_enter:%" PRIu64
		"\nn_drops_buffer_other_interest_exit:%" PRIu64
		"\nn_drops_buffer_close_exit:%" PRIu64
		"\nn_drops_buffer_proc_exit:%" PRIu64
		"\nn_drops_scratch_map:%" PRIu64
		"\nn_drops_pf:%" PRIu64
		"\nn_drops_bug:%" PRIu64
		"\n",
		stats.n_evts,
		stats.n_drops,
		stats.n_drops_buffer,
		stats.n_drops_buffer_clone_fork_enter,
		stats.n_drops_buffer_clone_fork_exit,
		stats.n_drops_buffer_execve_enter,
		stats.n_drops_buffer_execve_exit,
		stats.n_drops_buffer_connect_enter,
		stats.n_drops_buffer_connect_exit,
		stats.n_drops_buffer_open_enter,
		stats.n_drops_buffer_open_exit,
		stats.n_drops_buffer_dir_file_enter,
		stats.n_drops_buffer_dir_file_exit,
		stats.n_drops_buffer_other_interest_enter,
		stats.n_drops_buffer_other_interest_exit,
		stats.n_drops_buffer_close_exit,
		stats.n_drops_buffer_proc_exit,
		stats.n_drops_scratch_map,
		stats.n_drops_pf,
		stats.n_drops_bug);
}

const struct scap_stats_v2* sinsp::get_capture_stats_v2(uint32_t flags, uint32_t* nstats, int32_t* rc) const
{
	/* On purpose ignoring failures to not interrupt in case of stats retrieval failure. */
	const struct scap_stats_v2* stats_v2 = scap_get_stats_v2(m_h, flags, nstats, rc);
	if (!stats_v2)
	{
		*nstats = 0;
		return NULL;
	}
	return stats_v2;
}

#ifdef GATHER_INTERNAL_STATS
sinsp_stats sinsp::get_stats()
{
	scap_stats stats;

	//
	// Get capture stats from scap
	//
	if(m_h)
	{
		scap_get_stats(m_h, &stats);

		m_stats->m_n_seen_evts = stats.n_evts;
		m_stats->m_n_drops = stats.n_drops;
		m_stats->m_n_preemptions = stats.n_preemptions;
	}
	else
	{
		m_stats->m_n_seen_evts = 0;
		m_stats->m_n_drops = 0;
		m_stats->m_n_preemptions = 0;
	}

	//
	// Count the number of threads and fds by scanning the tables,
	// and update the thread-related stats.
	//
	if(m_thread_manager)
	{
		m_thread_manager->update_statistics();
	}

	//
	// Return the result
	//

	return *m_stats.get();
}
#endif // GATHER_INTERNAL_STATS

void sinsp::set_log_callback(sinsp_logger_callback cb)
{
	if(cb)
	{
		g_logger.add_callback_log(cb);
	}
	else
	{
		g_logger.remove_callback_log();
	}
}

void sinsp::set_log_file(std::string filename)
{
	g_logger.add_file_log(filename);
}

void sinsp::set_log_stderr()
{
	g_logger.add_stderr_log();
}

void sinsp::set_min_log_severity(sinsp_logger::severity sev)
{
	g_logger.set_severity(sev);
}

sinsp_evttables* sinsp::get_event_info_tables()
{
	return &g_infotables;
}

void sinsp::set_buffer_format(sinsp_evt::param_fmt format)
{
	m_buffer_format = format;
}

sinsp_evt::param_fmt sinsp::get_buffer_format()
{
	return m_buffer_format;
}

void sinsp::set_large_envs(bool enable)
{
	m_large_envs_enabled = enable;
}

void sinsp::set_debug_mode(bool enable_debug)
{
	m_isdebug_enabled = enable_debug;
}

void sinsp::set_print_container_data(bool print_container_data)
{
	m_print_container_data = print_container_data;
}

void sinsp::set_fatfile_dump_mode(bool enable_fatfile)
{
	m_isfatfile_enabled = enable_fatfile;
}

void sinsp::set_internal_events_mode(bool enable_internal_events)
{
	m_isinternal_events_enabled = enable_internal_events;
}

void sinsp::set_hostname_and_port_resolution_mode(bool enable)
{
	m_hostname_and_port_resolution_enabled = enable;
}

void sinsp::set_max_evt_output_len(uint32_t len)
{
	m_max_evt_output_len = len;
}

sinsp_protodecoder* sinsp::require_protodecoder(std::string decoder_name)
{
	return m_parser->add_protodecoder(decoder_name);
}

void sinsp::protodecoder_register_reset(sinsp_protodecoder* dec)
{
	m_decoders_reset_list.push_back(dec);
}

sinsp_parser* sinsp::get_parser()
{
	return m_parser;
}

bool sinsp::setup_cycle_writer(std::string base_file_name, int rollover_mb, int duration_seconds, int file_limit, unsigned long event_limit, bool compress)
{
	m_compress = compress;

	if(rollover_mb != 0 || duration_seconds != 0 || file_limit != 0 || event_limit != 0)
	{
		m_write_cycling = true;
	}

	return m_cycle_writer->setup(base_file_name, rollover_mb, duration_seconds, file_limit, event_limit);
}

double sinsp::get_read_progress_file()
{
	if(m_input_fd != 0)
	{
		// We can't get a reliable file size, so we can't get
		// any reliable progress
		return 0;
	}

	if(m_filesize == -1)
	{
		throw sinsp_exception(scap_getlasterr(m_h));
	}

	ASSERT(m_filesize != 0);

	int64_t fpos = scap_get_readfile_offset(m_h);

	if(fpos == -1)
	{
		throw sinsp_exception(scap_getlasterr(m_h));
	}

	return (double)fpos * 100 / m_filesize;
}

void sinsp::set_metadata_download_params(uint32_t data_max_b,
	uint32_t data_chunk_wait_us,
	uint32_t data_watch_freq_sec)
{
	m_metadata_download_params.m_data_max_b = data_max_b;
	m_metadata_download_params.m_data_chunk_wait_us = data_chunk_wait_us;
	m_metadata_download_params.m_data_watch_freq_sec = data_watch_freq_sec;
}

void sinsp::get_read_progress_plugin(OUT double* nres, std::string* sres)
{
	ASSERT(nres != NULL);
	ASSERT(sres != NULL);
	if(!nres || !sres)
	{
		return;
	}

	if (!m_input_plugin)
	{
		*nres = -1;
		*sres = "No Input Plugin";

		return;
	}

	uint32_t nplg;
	*sres = m_input_plugin->get_progress(nplg);

	*nres = ((double)nplg) / 100;
}

double sinsp::get_read_progress()
{
	if(is_plugin())
	{
		double res = 0;
		get_read_progress_plugin(&res, NULL);
		return res;
	}
	else
	{
		return get_read_progress_file();
	}
}

double sinsp::get_read_progress_with_str(OUT std::string* progress_str)
{
	if(is_plugin())
	{
		double res = 0;
		get_read_progress_plugin(&res, progress_str);
		return res;
	}
	else
	{
		*progress_str = "";
		return get_read_progress_file();
	}
}

bool sinsp::remove_inactive_threads()
{
	return m_thread_manager->remove_inactive_threads();
}

#if !defined(CYGWING_AGENT) && !defined(MINIMAL_BUILD) && !defined(__EMSCRIPTEN__)
void sinsp::init_mesos_client(std::string* api_server, bool verbose)
{
	m_verbose_json = verbose;
	if(m_mesos_client == NULL)
	{
		if(api_server)
		{
			// -m <url[,marathon_url]>
			std::string::size_type pos = api_server->find(',');
			if(pos != std::string::npos)
			{
				m_marathon_api_server.clear();
				m_marathon_api_server.push_back(api_server->substr(pos + 1));
			}
			m_mesos_api_server = api_server->substr(0, pos);
		}

		bool is_live = !m_mesos_api_server.empty();
		m_mesos_client = new mesos(m_mesos_api_server,
									m_marathon_api_server,
									true, // mesos leader auto-follow
									m_marathon_api_server.empty(), // marathon leader auto-follow if no uri
									mesos::credentials_t(), // mesos creds, the only way to provide creds is embedded in URI
									mesos::credentials_t(), // marathon creds
									mesos::default_timeout_ms,
									is_live,
									m_verbose_json);
	}
}

void sinsp::init_k8s_ssl(const std::string *ssl_cert)
{
#ifdef HAS_CAPTURE
	if(ssl_cert != nullptr && !ssl_cert->empty()
	   && (!m_k8s_ssl || ! m_k8s_bt))
	{
		std::string cert;
		std::string key;
		std::string key_pwd;
		std::string ca_cert;

		// -K <bt_file> | <cert_file>:<key_file[#password]>[:<ca_cert_file>]
		std::string::size_type pos = ssl_cert->find(':');
		if(pos == std::string::npos) // ca_cert-only is obsoleted, single entry is now bearer token
		{
			m_k8s_bt = std::make_shared<sinsp_bearer_token>(*ssl_cert);
		}
		else
		{
			cert = ssl_cert->substr(0, pos);
			if(cert.empty())
			{
				throw sinsp_exception(std::string("Invalid K8S SSL entry: ") + *ssl_cert);
			}

			// pos < ssl_cert->length() so it's safe to take
			// substr() from head, but it may be empty
			std::string::size_type head = pos + 1;
			pos = ssl_cert->find(':', head);
			if (pos == std::string::npos)
			{
				key = ssl_cert->substr(head);
			}
			else
			{
				key = ssl_cert->substr(head, pos - head);
				ca_cert = ssl_cert->substr(pos + 1);
			}
			if(key.empty())
			{
				throw sinsp_exception(std::string("Invalid K8S SSL entry: ") + *ssl_cert);
			}

			// Parse the password if it exists
			pos = key.find('#');
			if(pos != std::string::npos)
			{
				key_pwd = key.substr(pos + 1);
				key = key.substr(0, pos);
			}
		}
		g_logger.format(sinsp_logger::SEV_TRACE,
				"Creating sinsp_ssl with cert %s, key %s, key_pwd %s, ca_cert %s",
				cert.c_str(), key.c_str(), key_pwd.c_str(), ca_cert.c_str());
		m_k8s_ssl = std::make_shared<sinsp_ssl>(cert, key, key_pwd,
					ca_cert, ca_cert.empty() ? false : true, "PEM");
	}
#endif // HAS_CAPTURE
}

void sinsp::make_k8s_client()
{
	bool enable_capture = NULL != m_dumper && m_k8s_api_server && !m_k8s_api_server->empty();

	m_k8s_client = new k8s(
		
		// uri
		m_k8s_api_server ? *m_k8s_api_server : std::string()

		// For the k8s client, "is_captured" actually means: 
        // "Please, put k8s events data in a deque so we can consume them later."
		// 
		// is_captured
		,enable_capture

		// ssl
		// bt
		// block
#ifdef HAS_CAPTURE
		,m_k8s_ssl
		,m_k8s_bt
		,true // blocking
#endif // HAS_CAPTURE

		// event_filter
		,nullptr 

		// extensions
#ifdef HAS_CAPTURE
		,m_ext_list_ptr
#else
		,nullptr
#endif // HAS_CAPTURE

		// events_only
		,false 

		// node_selector
#ifdef HAS_CAPTURE
		,m_k8s_node_name ? *m_k8s_node_name : std::string()
#endif // HAS_CAPTURE
	);
}

void sinsp::init_k8s_client(std::string* api_server, std::string* ssl_cert, std::string* node_name, bool verbose)
{
	ASSERT(api_server);
	m_verbose_json = verbose;
	m_k8s_api_server = api_server;
	m_k8s_api_cert = ssl_cert;
	m_k8s_node_name = node_name;

#ifdef HAS_CAPTURE
	if(m_k8s_api_detected && m_k8s_ext_detect_done)
#endif // HAS_CAPTURE
	{
		if(m_k8s_client)
		{
			delete m_k8s_client;
			m_k8s_client = nullptr;
		}
		init_k8s_ssl(ssl_cert);
		make_k8s_client();
	}
}

void sinsp::validate_k8s_node_name()
{
	if(!m_k8s_node_name || m_k8s_node_name->size() == 0)
	{
		g_logger.log("No k8s node name passed as argument. "
			"This may result in performance penalty on large clusters", sinsp_logger::SEV_WARNING);
	}
	else 
	{
		bool found = false;
		const auto& state = m_k8s_client->get_state();

		for(const auto& node : state.get_nodes())
		{
			if(!node.get_node_name().compare(*m_k8s_node_name))
			{
				found = true;
				break;
			} 
		}

		if(!found)
		{
			// todo(jasondellaluce): we used to throw an exception here, however
			// at this point we have no guarantee on whether the provided node
			// name is wrong or if there was a failure in the k8s client event
			// parsing logic. As such, it's unsafe to throw an exception that
			// can potentially terminate the libs consumer. For now, we stick
			// to error-level logging, however this is an issue we may want to
			// further investigate in the future.
			// see: https://github.com/falcosecurity/falco/issues/2358
			g_logger.log(
				"Failing to enrich events with Kubernetes metadata: "
				"node name does not correspond to a node in the cluster: "
				+ *m_k8s_node_name, sinsp_logger::SEV_ERROR);
		}
	}

	m_k8s_node_name_validated = true;
}

void sinsp::collect_k8s()
{
	if(m_parser)
	{
		if(m_k8s_api_server)
		{
			if(!m_k8s_client)
			{
				init_k8s_client(m_k8s_api_server, m_k8s_api_cert, m_k8s_node_name, m_verbose_json);
				if(m_k8s_client)
				{
					g_logger.log("K8s client created.", sinsp_logger::SEV_DEBUG);
				}
				else
				{
					g_logger.log("K8s client NOT created.", sinsp_logger::SEV_DEBUG);
				}
			}
			if(m_k8s_client)
			{
				if(m_lastevent_ts >
					m_k8s_last_watch_time_ns + (m_metadata_download_params.m_data_watch_freq_sec * ONE_SECOND_IN_NS))
				{
					m_k8s_last_watch_time_ns = m_lastevent_ts;
					g_logger.log("K8s updating state ...", sinsp_logger::SEV_DEBUG);
					uint64_t delta = sinsp_utils::get_current_time_ns();
					m_k8s_client->watch();
					m_parser->schedule_k8s_events();
					delta = sinsp_utils::get_current_time_ns() - delta;
					g_logger.format(sinsp_logger::SEV_DEBUG, "Updating Kubernetes state took %" PRIu64 " ms", delta / 1000000LL);
				}

				if(!m_k8s_node_name_validated)
				{
					validate_k8s_node_name();
				}
			}
		}
	}
}

void sinsp::k8s_discover_ext()
{
#ifdef HAS_CAPTURE
	try
	{
		if(m_k8s_api_server && !m_k8s_api_server->empty() && !m_k8s_ext_detect_done)
		{
			g_logger.log("K8s API extensions handler: detecting extensions.", sinsp_logger::SEV_TRACE);
			if(!m_k8s_ext_handler)
			{
				if(!m_k8s_collector)
				{
					m_k8s_collector = std::make_shared<k8s_handler::collector_t>();
				}
				if(uri(*m_k8s_api_server).is_secure()) { init_k8s_ssl(m_k8s_api_cert); }
				m_k8s_ext_handler.reset(new k8s_api_handler(m_k8s_collector, *m_k8s_api_server,
									    "/apis/apps/v1", "[.resources[].name]",
									    "1.1", m_k8s_ssl, m_k8s_bt, true));
				g_logger.log("K8s API extensions handler: collector created.", sinsp_logger::SEV_TRACE);
			}
			else
			{
				g_logger.log("K8s API extensions handler: collecting data.", sinsp_logger::SEV_TRACE);
				m_k8s_ext_handler->collect_data();
				if(m_k8s_ext_handler->ready())
				{
					g_logger.log("K8s API extensions handler: data received.", sinsp_logger::SEV_TRACE);
					if(m_k8s_ext_handler->error())
					{
						g_logger.log("K8s API extensions handler: data error occurred while detecting API extensions.",
									 sinsp_logger::SEV_WARNING);
						m_ext_list_ptr.reset();
					}
					else
					{
						const k8s_api_handler::api_list_t& exts = m_k8s_ext_handler->extensions();
						std::ostringstream ostr;
						k8s_ext_list_t ext_list;
						for(const auto& ext : exts)
						{
							if (m_k8s_allowed_ext.find(ext) == m_k8s_allowed_ext.end()) 
							{
								// skip not allowed extension
								continue;
							} 
							ext_list.insert(ext);
							ostr << std::endl << ext;
						}
						g_logger.log("K8s API extensions handler extensions found: " + ostr.str(),
									 sinsp_logger::SEV_DEBUG);
						m_ext_list_ptr.reset(new k8s_ext_list_t(ext_list));
					}
					m_k8s_ext_detect_done = true;
					m_k8s_collector.reset();
					m_k8s_ext_handler.reset();
				}
				else
				{
					g_logger.log("K8s API extensions handler: not ready.", sinsp_logger::SEV_TRACE);
				}
			}
		}
	}
	catch(const std::exception& ex)
	{
		g_logger.log(std::string("K8s API extensions handler error: ").append(ex.what()),
					 sinsp_logger::SEV_ERROR);
		m_k8s_ext_detect_done = false;
		m_k8s_collector.reset();
		m_k8s_ext_handler.reset();
	}
	g_logger.log("K8s API extensions handler: detection done.", sinsp_logger::SEV_TRACE);
#endif // HAS_CAPTURE
}

void sinsp::update_k8s_state()
{
#ifdef HAS_CAPTURE
	try
	{
		if(m_k8s_api_server && !m_k8s_api_server->empty())
		{
			if(!m_k8s_api_detected)
			{
				if(!m_k8s_api_handler)
				{
					if(!m_k8s_collector)
					{
						m_k8s_collector = std::make_shared<k8s_handler::collector_t>();
					}
					if(uri(*m_k8s_api_server).is_secure() && (!m_k8s_ssl || ! m_k8s_bt))
					{
						init_k8s_ssl(m_k8s_api_cert);
					}
					m_k8s_api_handler.reset(new k8s_api_handler(m_k8s_collector, *m_k8s_api_server,
										    "/api", ".versions", "1.1",
										    m_k8s_ssl, m_k8s_bt, true,
										    m_metadata_download_params.m_data_max_b,
										    m_metadata_download_params.m_data_chunk_wait_us));
				}
				else
				{
					m_k8s_api_handler->collect_data();
					if(m_k8s_api_handler->ready())
					{
						g_logger.log("K8s API handler data received.", sinsp_logger::SEV_DEBUG);
						if(m_k8s_api_handler->error())
						{
							g_logger.log("K8s API handler data error occurred while detecting API versions.",
										 sinsp_logger::SEV_ERROR);
						}
						else
						{
							m_k8s_api_detected = m_k8s_api_handler->has("v1");
							if(m_k8s_api_detected)
							{
								g_logger.log("K8s API server v1 detected.", sinsp_logger::SEV_DEBUG);
							}
						}
						m_k8s_collector.reset();
						m_k8s_api_handler.reset();
					}
					else
					{
						g_logger.log("K8s API handler not ready yet.", sinsp_logger::SEV_DEBUG);
					}
				}
			}
			if(m_k8s_api_detected && !m_k8s_ext_detect_done)
			{
				k8s_discover_ext();
			}
			if(m_k8s_api_detected && m_k8s_ext_detect_done)
			{
				collect_k8s();
			}
		}
	}
	catch(const std::exception& e)
	{
		g_logger.log(std::string("Error fetching K8s data: ").append(e.what()), sinsp_logger::SEV_ERROR);
		throw;
	}
#endif // HAS_CAPTURE
}

bool sinsp::get_mesos_data()
{
	bool ret = false;
#ifdef HAS_CAPTURE
	try
	{
		static time_t last_mesos_refresh = 0;
		ASSERT(m_mesos_client);
		ASSERT(m_mesos_client->is_alive());

		time_t now; time(&now);
		if(last_mesos_refresh)
		{
			g_logger.log("Collecting Mesos data ...", sinsp_logger::SEV_DEBUG);
			ret = m_mesos_client->collect_data();
		}
		if(difftime(now, last_mesos_refresh) > 10)
		{
			g_logger.log("Requesting Mesos data ...", sinsp_logger::SEV_DEBUG);
			m_mesos_client->send_data_request(false);
			last_mesos_refresh = now;
		}
	}
	catch(const std::exception& ex)
	{
		g_logger.log(std::string("Mesos exception: ") + ex.what(), sinsp_logger::SEV_ERROR);
		delete m_mesos_client;
		m_mesos_client = NULL;
		init_mesos_client(0, m_verbose_json);
	}
#endif // HAS_CAPTURE
	return ret;
}

void sinsp::update_mesos_state()
{
	ASSERT(m_mesos_client);
	if(m_lastevent_ts >
		m_mesos_last_watch_time_ns + (m_metadata_download_params.m_data_watch_freq_sec * ONE_SECOND_IN_NS))
	{
		m_mesos_last_watch_time_ns = m_lastevent_ts;
		if(m_mesos_client->is_alive())
		{
			uint64_t delta = sinsp_utils::get_current_time_ns();
			if(m_parser && get_mesos_data())
			{
				m_parser->schedule_mesos_events();
				delta = sinsp_utils::get_current_time_ns() - delta;
				g_logger.format(sinsp_logger::SEV_DEBUG, "Updating Mesos state took %" PRIu64 " ms", delta / 1000000LL);
			}
		}
		else
		{
			g_logger.format(sinsp_logger::SEV_ERROR, "Mesos connection not active anymore, retrying ...");
			delete m_mesos_client;
			m_mesos_client = NULL;
			init_mesos_client(0, m_verbose_json);
		}
	}
}
#endif // CYGWING_AGENT

void sinsp::disable_automatic_threadtable_purging()
{
	m_automatic_threadtable_purging = false;
}

void sinsp::set_thread_purge_interval_s(uint32_t val)
{
	m_inactive_thread_scan_time_ns = (uint64_t)val * ONE_SECOND_IN_NS;
}

void sinsp::set_thread_timeout_s(uint32_t val)
{
	m_thread_timeout_ns = (uint64_t)val * ONE_SECOND_IN_NS;
}

void sinsp::set_proc_scan_timeout_ms(uint64_t val)
{
	m_proc_scan_timeout_ms = val;
}

void sinsp::set_proc_scan_log_interval_ms(uint64_t val)
{
	m_proc_scan_log_interval_ms = val;
}

///////////////////////////////////////////////////////////////////////////////
// Note: this is defined here so we can inline it in sinso::next
///////////////////////////////////////////////////////////////////////////////

/* Returns true when we scan the table */
bool sinsp_thread_manager::remove_inactive_threads()
{
	if(m_last_flush_time_ns == 0)
	{
		//
		// Set the first table scan for 30 seconds in, so that we can spot bugs in the logic without having
		// to wait for tens of minutes
		//
		if(m_inspector->m_inactive_thread_scan_time_ns > 30 * ONE_SECOND_IN_NS)
		{
			m_last_flush_time_ns =
				(m_inspector->m_lastevent_ts - m_inspector->m_inactive_thread_scan_time_ns + 30 * ONE_SECOND_IN_NS);
		}
		else
		{
			m_last_flush_time_ns =
				(m_inspector->m_lastevent_ts - m_inspector->m_inactive_thread_scan_time_ns);
		}
	}

	if(m_inspector->m_lastevent_ts >
		m_last_flush_time_ns + m_inspector->m_inactive_thread_scan_time_ns)
	{
		std::unordered_set<int64_t> to_delete;

		m_last_flush_time_ns = m_inspector->m_lastevent_ts;

		g_logger.format(sinsp_logger::SEV_INFO, "Flushing thread table");

		/* Here we loop over the table in search of threads to delete. We remove:
		 * 1. Invalid threads.
		 * 2. Threads that we are not using and that are no more alive in /proc.
		 */
		m_threadtable.loop([&] (sinsp_threadinfo& tinfo) {
			if(tinfo.is_invalid() ||
				((m_inspector->m_lastevent_ts > tinfo.m_lastaccess_ts + m_inspector->m_thread_timeout_ns) &&
					!scap_is_thread_alive(m_inspector->m_h, tinfo.m_pid, tinfo.m_tid, tinfo.m_comm.c_str())))
			{
				to_delete.insert(tinfo.m_tid);
			}
			return true;
		});

		for(const auto& tid_to_remove : to_delete)
		{
			remove_thread(tid_to_remove);
		}

		/* Clean expired threads in the group and children */
		reset_child_dependencies();
		return true;
	}

	return false;
}

sinsp_threadinfo*
libsinsp::event_processor::build_threadinfo(sinsp* inspector)
{
	return new sinsp_threadinfo(inspector);
}

void sinsp::handle_plugin_async_event(const sinsp_plugin& p, std::unique_ptr<sinsp_evt> evt)
{
	// note: this function can be invoked from different plugin threads,
	// so we need to make sure that every variable we read is either constant
	// during the lifetime of those threads, or that it is atomic.

	// note: we make sure that async events are dequeued, however
	// they are considered only during live captures, because
	// offline captures will have the async events already encoded
	// in the event stream.
	if (!is_capture())
	{
		// note: async events get assigned the same event source as the
		// currently-open one. In case no plugin is open, we set it to zero
		// to indicate the regular "syscall" source. Otherwise we set the ID
		// to the one of the currently-open ID, which can potentially be zero
		// too in case the plugin does not define any specific event source.
		// However, we also need to check if the plugin's async capability
		// is compatible with the currently-open event source, and reject
		// the event if that's not the case.
		//
		// todo(jasondellaluce): here we are assuming that the "syscall" event
		// source is always at index 0 in the inspector's event source list,
		// change this code if this assumption ever stops being true.
		size_t cur_evtsrc_idx = 0;
		uint32_t cur_plugin_id = 0;
		if (is_plugin())
		{
			cur_plugin_id = m_input_plugin->id();
			if (cur_plugin_id != 0)
			{
				bool found = false;
				cur_evtsrc_idx = m_plugin_manager->source_idx_by_plugin_id(cur_plugin_id, found);
				if (!found)
				{
					throw sinsp_exception("can't find event source for plugin ID: "
						+ std::to_string(cur_plugin_id));
				}
			}
		}
		ASSERT(cur_evtsrc_idx < m_event_sources.size());
		const auto& cur_evtsrc = m_event_sources[cur_evtsrc_idx];
		if (!sinsp_plugin::is_source_compatible(p.async_event_sources(), cur_evtsrc))
		{
			throw sinsp_exception("async events of plugin '" + p.name()
				+ "' are not compatible with open event source '" + cur_evtsrc + "'");
		}

		// if the async event is generated by a non-syscall event source, then
		// async events must have no thread associated.
		if (cur_plugin_id != 0 && evt->m_pevt->tid != (uint64_t) -1)
		{
			throw sinsp_exception("async events of plugin '" + p.name()
				+ "' can have no thread associated with open event source '" + cur_evtsrc + "'");
		}

		// write plugin ID and timestamp in the event and kick it in the queue
		auto plid = (uint32_t*)((uint8_t*) evt->m_pevt + sizeof(scap_evt) + 4+4+4);
		*plid = cur_plugin_id;
		evt->inspector(this);
		evt->m_pevt->ts = (uint64_t) -1;
#ifndef __EMSCRIPTEN__				
		m_pending_state_evts.push(std::move(evt));
#endif
	}
}

bool sinsp::get_track_connection_status()
{
	return m_parser->get_track_connection_status();
}

void sinsp::set_track_connection_status(bool enabled)
{
	m_parser->set_track_connection_status(enabled);
}

uint64_t sinsp::get_new_ts()
{
	// m_lastevent_ts = 0 at startup when containers are
	// being created as a part of the initial process
	// scan.
	return (m_lastevent_ts == 0)
			? sinsp_utils::get_current_time_ns()
			: m_lastevent_ts;
}
