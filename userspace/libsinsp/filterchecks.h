/*
Copyright (C) 2021 The Falco Authors.

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

#pragma once
#include <optional>
#include <string>
#include <unordered_set>
#include <json/json.h>
#include <set>
#include "filter_value.h"
#include "prefix_search.h"
#if !defined(CYGWING_AGENT) && !defined(MINIMAL_BUILD) && !defined(__EMSCRIPTEN__)
#include "k8s.h"
#include "mesos.h"
#endif
#include "sinsp.h"

#include "gen_filter.h"

class sinsp_filter_check_reference;

bool flt_compare(cmpop op, ppm_param_type type, void* operand1, void* operand2, uint32_t op1_len = 0, uint32_t op2_len = 0);
bool flt_compare_avg(cmpop op, ppm_param_type type, void* operand1, void* operand2, uint32_t op1_len, uint32_t op2_len, uint32_t cnt1, uint32_t cnt2);
bool flt_compare_ipv4net(cmpop op, uint64_t operand1, const ipv4net* operand2);
bool flt_compare_ipv6net(cmpop op, const ipv6addr *operand1, const ipv6net *operand2);

char* flt_to_string(uint8_t* rawval, filtercheck_field_info* finfo);
int32_t gmt2local(time_t t);

class operand_info
{
public:
	uint32_t m_id;
	ppm_param_type m_type;
	std::string m_name;
	std::string m_description;
};

class check_extraction_cache_entry
{
public:
	uint64_t m_evtnum = UINT64_MAX;
	std::vector<extract_value_t> m_res;
};

class check_eval_cache_entry
{
public:
	uint64_t m_evtnum = UINT64_MAX;
	bool m_res;
};

class check_cache_metrics
{
public:
	// The number of times extract_cached() was called
	uint64_t m_num_extract;

	// The number of times extract_cached() could use a cached value
	uint64_t m_num_extract_cache;

	// The number of times compare() was called
	uint64_t m_num_eval;

	// The number of times compare() could use a cached value
	uint64_t m_num_eval_cache;
};

///////////////////////////////////////////////////////////////////////////////
// The filter check interface
// NOTE: in order to add a new type of filter check, you need to add a class for
//       it and then add it to new_filter_check_from_name.
///////////////////////////////////////////////////////////////////////////////

class sinsp_filter_check : public gen_event_filter_check
{
public:
	sinsp_filter_check();

	virtual ~sinsp_filter_check()
	{
	}

	//
	// Allocate a new check of the same type.
	// Every filtercheck plugin must implement this.
	//
	virtual sinsp_filter_check* allocate_new() = 0;

	//
	// Get the list of fields that this check exports
	//
	virtual filter_check_info* get_fields()
	{
		return &m_info;
	}

	//
	// Parse the name of the field.
	// Returns the length of the parsed field if successful, an exception in
	// case of error.
	//
	virtual int32_t parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering);

	//
	// If this check is used by a filter, extract the constant to compare it to
	// Doesn't return the field length because the filtering engine can calculate it.
	//
	void add_filter_value(const char* str, uint32_t len, uint32_t i = 0 );
	virtual size_t parse_filter_value(const char* str, uint32_t len, uint8_t *storage, uint32_t storage_len);

	//
	// Called after parsing for optional validation of the filter value
	//
	void validate_filter_value(const char* str, uint32_t len) {}

	//
	// Return the info about the field that this instance contains
	//
	virtual const filtercheck_field_info* get_field_info();

	//
	// Extract the field from the event. In sanitize_strings is true, any
	// string values are sanitized to remove nonprintable characters.
	//
	bool extract(gen_event *evt, OUT std::vector<extract_value_t>& values, bool sanitize_strings = true);

	// Alias of extract that uses the sinsp_evt type.
	// By default, this fills the vector with only one value, retireved by calling the single-result
	// extract method.
	// If a NULL value is returned by extract, the vector is emptied.
	// Subclasses are meant to either override this, or the single-valued extract method.
	virtual bool extract(sinsp_evt *evt, OUT std::vector<extract_value_t>& values, bool sanitize_strings = true);

	//
	// Wrapper for extract() that implements caching to speed up multiple extractions of the same value,
	// which are common in Falco.
	//
	bool extract_cached(sinsp_evt *evt, OUT std::vector<extract_value_t>& values, bool sanitize_strings = true);

	//
	// Extract the field as json from the event (by default, fall
	// back to the regular extract functionality)
	//
	virtual Json::Value extract_as_js(sinsp_evt *evt, OUT uint32_t* len)
	{
		return Json::nullValue;
	}

	//
	// Compare the field with the constant value obtained from parse_filter_value()
	//
	bool compare(gen_event *evt);
	virtual bool compare(sinsp_evt *evt);

	//
	// Extract the value from the event and convert it into a string
	//
	virtual char* tostring(sinsp_evt* evt);

	//
	// Extract the value from the event and convert it into a Json value
	// or object
	//
	virtual Json::Value tojson(sinsp_evt* evt);

	sinsp* m_inspector;
	bool m_needs_state_tracking = false;
	check_eval_cache_entry* m_eval_cache_entry = NULL;
	check_extraction_cache_entry* m_extraction_cache_entry = NULL;
	std::vector<extract_value_t> m_extracted_values;
	check_cache_metrics *m_cache_metrics = NULL;

protected:
	// This is a single-value version of extract for subclasses non supporting extracting
	// multiple values. By default, this returns NULL.
	// Subclasses are meant to either override this, or the multi-valued extract method.
	virtual uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);
	
	bool flt_compare(cmpop op, ppm_param_type type, void* operand1, uint32_t op1_len = 0, uint32_t op2_len = 0);
	bool flt_compare(cmpop op, ppm_param_type type, std::vector<extract_value_t>& values, uint32_t op2_len = 0);

	char* rawval_to_string(uint8_t* rawval,
			       ppm_param_type ptype,
			       ppm_print_format print_format,
			       uint32_t len);
	Json::Value rawval_to_json(uint8_t* rawval, ppm_param_type ptype, ppm_print_format print_format, uint32_t len);
	void string_to_rawval(const char* str, uint32_t len, ppm_param_type ptype);

	char m_getpropertystr_storage[1024];
	std::vector<std::vector<uint8_t>> m_val_storages;
	inline uint8_t* filter_value_p(uint16_t i = 0) { return &m_val_storages[i][0]; }
	inline std::vector<uint8_t>* filter_value(uint16_t i = 0) { return &m_val_storages[i]; }

	std::vector<filter_value_t> m_vals;

	std::unordered_set<filter_value_t,
		g_hash_membuf,
		g_equal_to_membuf> m_val_storages_members;

	path_prefix_search m_val_storages_paths;

	uint32_t m_val_storages_min_size;
	uint32_t m_val_storages_max_size;

	const filtercheck_field_info* m_field;
	filter_check_info m_info;
	uint32_t m_field_id;
	uint32_t m_val_storage_len;

private:
	void set_inspector(sinsp* inspector);

friend class filter_check_list;
friend class sinsp_filter_optimizer;
friend class chk_compare_helper;
};

///////////////////////////////////////////////////////////////////////////////
// Filter check classes
///////////////////////////////////////////////////////////////////////////////

//
// fd checks
//
class sinsp_filter_check_fspath : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_NAME = 0,
		TYPE_NAMERAW = 1,
		TYPE_SOURCE = 2,
		TYPE_SOURCERAW = 3,
		TYPE_TARGET = 4,
		TYPE_TARGETRAW = 5,
	};

	sinsp_filter_check_fspath();
	sinsp_filter_check* allocate_new();
	uint8_t* extract(sinsp_evt* evt, OUT uint32_t* len, bool sanitize_strings = true);

private:
	typedef std::map<uint16_t, std::shared_ptr<sinsp_filter_check>> filtercheck_map_t;

	std::shared_ptr<sinsp_filter_check> create_event_check(const char *name,
							       cmpop cop = CO_NONE,
							       const char *value = NULL);

	std::shared_ptr<sinsp_filter_check> create_fd_check(const char *name);

	void create_fspath_checks();
	void set_fspath_checks(std::shared_ptr<filtercheck_map_t> success_checks,
			       std::shared_ptr<filtercheck_map_t> path_checks,
			       std::shared_ptr<filtercheck_map_t> source_checks,
			       std::shared_ptr<filtercheck_map_t> target_checks);
	bool extract_fspath(sinsp_evt* evt,
			    OUT std::vector<extract_value_t>& values,
			    std::shared_ptr<filtercheck_map_t> map);
	std::string m_tstr;
	sinsp_evt m_tmp_evt;

	std::shared_ptr<filtercheck_map_t> m_success_checks;
	std::shared_ptr<filtercheck_map_t> m_path_checks;
	std::shared_ptr<filtercheck_map_t> m_source_checks;
	std::shared_ptr<filtercheck_map_t> m_target_checks;
};

//
// fd checks
//
class sinsp_filter_check_fd : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_FDNUM = 0,
		TYPE_FDTYPE = 1,
		TYPE_FDTYPECHAR = 2,
		TYPE_FDNAME = 3,
		TYPE_DIRECTORY = 4,
		TYPE_FILENAME = 5,
		TYPE_IP = 6,
		TYPE_CLIENTIP = 7,
		TYPE_SERVERIP = 8,
		TYPE_LIP = 9,
		TYPE_RIP = 10,
		TYPE_PORT = 11,
		TYPE_CLIENTPORT = 12,
		TYPE_SERVERPORT = 13,
		TYPE_LPORT = 14,
		TYPE_RPORT = 15,
		TYPE_L4PROTO = 16,
		TYPE_SOCKFAMILY = 17,
		TYPE_IS_SERVER = 18,
		TYPE_UID = 19,
		TYPE_CONTAINERNAME = 20,
		TYPE_CONTAINERDIRECTORY = 21,
		TYPE_PROTO = 22,
		TYPE_CLIENTPROTO = 23,
		TYPE_SERVERPROTO = 24,
		TYPE_LPROTO = 25,
		TYPE_RPROTO = 26,
		TYPE_NET = 27,
		TYPE_CNET = 28,
		TYPE_SNET = 29,
		TYPE_LNET = 30,
		TYPE_RNET = 31,
		TYPE_IS_CONNECTED = 32,
		TYPE_NAME_CHANGED = 33,
		TYPE_CLIENTIP_NAME = 34,
		TYPE_SERVERIP_NAME = 35,
		TYPE_LIP_NAME = 36,
		TYPE_RIP_NAME = 37,
		TYPE_DEV = 38,
		TYPE_DEV_MAJOR = 39,
		TYPE_DEV_MINOR = 40,
		TYPE_INO = 41,
		TYPE_FDNAMERAW = 42,
		TYPE_FDTYPES = 43,
	};

	enum fd_type
	{
		FDT_NONE,
		FDT_FILE,
		FDT_SOCK,
		FDT_IPV4_SOCK,
		FDT_IPV6_SOCK,
		FDT_UNIX_SOCK,
		FDT_PIPE,
		FDT_EVENT,
		FDT_SIGNALFD,
		FDT_EVENTPOLL,
		FDT_INOTIFY,
		FDT_TIMERFD
	};

	sinsp_filter_check_fd();
	sinsp_filter_check* allocate_new();
	int32_t parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering);
	bool extract(sinsp_evt *evt, OUT std::vector<extract_value_t>& values, bool sanitize_strings = true);
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);
	int32_t extract_arg(std::string fldname, std::string val);
	bool compare_ip(sinsp_evt *evt);
	bool compare_net(sinsp_evt *evt);
	bool compare_port(sinsp_evt *evt);
	bool compare_domain(sinsp_evt *evt);
	bool compare(sinsp_evt *evt);

	sinsp_threadinfo* m_tinfo;
	sinsp_fdinfo_t* m_fdinfo;
	fd_type m_fd_type;
	std::string m_tstr;
	uint8_t m_tcstr[2];
	uint32_t m_tbool;
	int64_t m_argid;

	/* Used in extract helper to save uint64_t data */
	uint64_t m_conv_uint64;

private:
	uint8_t* extract_from_null_fd(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings);
	bool extract_fdname_from_creator(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings, bool fd_nameraw = false);
	bool extract_fd(sinsp_evt *evt);
};

//
// thread sinsp_filter_check_thread
//
class sinsp_filter_check_thread : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_EXE = 0,
		TYPE_PEXE,
		TYPE_AEXE,
		TYPE_EXEPATH,
		TYPE_PEXEPATH,
		TYPE_AEXEPATH,
		TYPE_NAME,
		TYPE_PNAME,
		TYPE_ANAME,
		TYPE_ARGS,
		TYPE_CMDLINE,
		TYPE_PCMDLINE,
		TYPE_ACMDLINE,
		TYPE_CMDNARGS,
		TYPE_CMDLENARGS,
		TYPE_EXELINE,
		TYPE_ENV,
		TYPE_CWD,
		TYPE_LOGINSHELLID,
		TYPE_TTY,
		TYPE_PID,
		TYPE_PPID,
		TYPE_APID,
		TYPE_VPID,
		TYPE_PVPID,
		TYPE_SID,
		TYPE_SNAME,
		TYPE_SID_EXE,
		TYPE_SID_EXEPATH,
		TYPE_VPGID,
		TYPE_VPGID_NAME,
		TYPE_VPGID_EXE,
		TYPE_VPGID_EXEPATH,
		TYPE_DURATION,
		TYPE_PPID_DURATION,
		TYPE_PID_CLONE_TS,
		TYPE_PPID_CLONE_TS,
		TYPE_IS_EXE_WRITABLE,
		TYPE_IS_EXE_UPPER_LAYER,
		TYPE_IS_EXE_FROM_MEMFD,
		TYPE_IS_SID_LEADER,
		TYPE_IS_VPGID_LEADER,
		TYPE_EXE_INO,
		TYPE_EXE_INO_CTIME,
		TYPE_EXE_INO_MTIME,
		TYPE_EXE_INO_CTIME_DURATION_CLONE_TS,
		TYPE_EXE_INO_CTIME_DURATION_PIDNS_START,
		TYPE_PIDNS_INIT_START_TS,
		TYPE_CAP_PERMITTED,
		TYPE_CAP_INHERITABLE,
		TYPE_CAP_EFFECTIVE,
		TYPE_IS_CONTAINER_HEALTHCHECK,
		TYPE_IS_CONTAINER_LIVENESS_PROBE,
		TYPE_IS_CONTAINER_READINESS_PROBE,
		TYPE_FDOPENCOUNT,
		TYPE_FDLIMIT,
		TYPE_FDUSAGE,
		TYPE_VMSIZE,
		TYPE_VMRSS,
		TYPE_VMSWAP,
		TYPE_PFMAJOR,
		TYPE_PFMINOR,
		TYPE_TID,
		TYPE_ISMAINTHREAD,
		TYPE_VTID,
		TYPE_NAMETID,
		TYPE_EXECTIME,
		TYPE_TOTEXECTIME,
		TYPE_CGROUPS,
		TYPE_CGROUP,
		TYPE_NTHREADS,
		TYPE_NCHILDS,
		TYPE_THREAD_CPU,
		TYPE_THREAD_CPU_USER,
		TYPE_THREAD_CPU_SYSTEM,
		TYPE_THREAD_VMSIZE,
		TYPE_THREAD_VMRSS,
		TYPE_THREAD_VMSIZE_B,
		TYPE_THREAD_VMRSS_B,
	};

	sinsp_filter_check_thread();
	sinsp_filter_check* allocate_new();
	int32_t parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering);
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);
	bool compare(sinsp_evt *evt);

	int32_t get_argid();

private:
	uint64_t extract_exectime(sinsp_evt *evt);
	int32_t extract_arg(std::string fldname, std::string val, OUT const struct ppm_param_info** parinfo);
	uint8_t* extract_thread_cpu(sinsp_evt *evt, OUT uint32_t* len, sinsp_threadinfo* tinfo, bool extract_user, bool extract_system);
	inline bool compare_full_apid(sinsp_evt *evt);
	bool compare_full_aname(sinsp_evt *evt);
	bool compare_full_aexe(sinsp_evt *evt);
	bool compare_full_aexepath(sinsp_evt *evt);
	bool compare_full_acmdline(sinsp_evt *evt);
	bool filter_proc(sinsp_evt *evt);

	int32_t m_argid;
	std::string m_argname;
	uint32_t m_tbool;
	std::string m_tstr;
	uint64_t m_u64val;
	int64_t m_s64val;
	double m_dval;
	std::vector<uint64_t> m_last_proc_switch_times;
	uint64_t m_cursec_ts;
	std::unique_ptr<libsinsp::state::dynamic_struct::field_accessor<uint64_t>> m_thread_dyn_field_accessor;
	std::set<int64_t> m_proc_set;
};

//
// filterchecks that will work on any generic event
//
class sinsp_filter_check_gen_event : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_NUMBER = 0,
		TYPE_TIME = 1,
		TYPE_TIME_S = 2,
		TYPE_TIME_ISO8601 = 3,
		TYPE_DATETIME = 4,
		TYPE_DATETIME_S = 5,
		TYPE_RAWTS = 6,
		TYPE_RAWTS_S = 7,
		TYPE_RAWTS_NS = 8,
		TYPE_RELTS = 9,
		TYPE_RELTS_S = 10,
		TYPE_RELTS_NS = 11,
		TYPE_PLUGINNAME = 12,
		TYPE_PLUGININFO = 13,
		TYPE_SOURCE = 14,
		TYPE_ISASYNC = 15,
		TYPE_ASYNCTYPE = 16,
		TYPE_HOSTNAME = 17,
	};

	sinsp_filter_check_gen_event();
	~sinsp_filter_check_gen_event();
	sinsp_filter_check* allocate_new();
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);
	Json::Value extract_as_js(sinsp_evt *evt, OUT uint32_t* len);

	uint64_t m_u64val;
	uint32_t m_u32val;
	std::string m_strstorage;
};

//
// event checks
//
class sinsp_filter_check_event : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_LATENCY = 0,
		TYPE_LATENCY_S = 1,
		TYPE_LATENCY_NS = 2,
		TYPE_LATENCY_QUANTIZED = 3,
		TYPE_LATENCY_HUMAN = 4,
		TYPE_DELTA = 5,
		TYPE_DELTA_S = 6,
		TYPE_DELTA_NS = 7,
		TYPE_RUNTIME_TIME_OUTPUT_FORMAT = 8,
		TYPE_DIR = 9,
		TYPE_TYPE = 10,
		TYPE_TYPE_IS = 11,
		TYPE_SYSCALL_TYPE = 12,
		TYPE_CATEGORY = 13,
		TYPE_CPU = 14,
		TYPE_ARGS = 15,
		TYPE_ARGSTR = 16,
		TYPE_ARGRAW = 17,
		TYPE_INFO = 18,
		TYPE_BUFFER = 19,
		TYPE_BUFLEN = 20,
		TYPE_RESSTR = 21,
		TYPE_RESRAW = 22,
		TYPE_FAILED = 23,
		TYPE_ISIO = 24,
		TYPE_ISIO_READ = 25,
		TYPE_ISIO_WRITE = 26,
		TYPE_IODIR = 27,
		TYPE_ISWAIT = 28,
		TYPE_WAIT_LATENCY = 29,
		TYPE_ISSYSLOG = 30,
		TYPE_COUNT = 31,
		TYPE_COUNT_ERROR = 32,
		TYPE_COUNT_ERROR_FILE = 33,
		TYPE_COUNT_ERROR_NET = 34,
		TYPE_COUNT_ERROR_MEMORY = 35,
		TYPE_COUNT_ERROR_OTHER = 36,
		TYPE_COUNT_EXIT = 37,
		TYPE_COUNT_PROCINFO = 38,
		TYPE_COUNT_THREADINFO = 39,
		TYPE_AROUND = 40,
		TYPE_ABSPATH = 41,
		TYPE_BUFLEN_IN = 42,
		TYPE_BUFLEN_OUT = 43,
		TYPE_BUFLEN_FILE = 44,
		TYPE_BUFLEN_FILE_IN = 45,
		TYPE_BUFLEN_FILE_OUT = 46,
		TYPE_BUFLEN_NET = 47,
		TYPE_BUFLEN_NET_IN = 48,
		TYPE_BUFLEN_NET_OUT = 49,
		TYPE_ISOPEN_READ = 50,
		TYPE_ISOPEN_WRITE = 51,
		TYPE_INFRA_DOCKER_NAME = 52,
		TYPE_INFRA_DOCKER_CONTAINER_ID = 53,
		TYPE_INFRA_DOCKER_CONTAINER_NAME = 54,
		TYPE_INFRA_DOCKER_CONTAINER_IMAGE = 55,
		TYPE_ISOPEN_EXEC = 56,
		TYPE_ISOPEN_CREATE = 57,
	};

	sinsp_filter_check_event();
	~sinsp_filter_check_event();
	sinsp_filter_check* allocate_new();
	int32_t parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering);
	size_t parse_filter_value(const char* str, uint32_t len, uint8_t *storage, uint32_t storage_len);
	void validate_filter_value(const char* str, uint32_t len);
	const filtercheck_field_info* get_field_info();
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);
	Json::Value extract_as_js(sinsp_evt *evt, OUT uint32_t* len);
	bool compare(sinsp_evt *evt);

	uint64_t m_u64val;
	uint64_t m_tsdelta;
	uint32_t m_u32val;
	std::string m_strstorage;
	std::string m_argname;
	int32_t m_argid;
	uint32_t m_evtid;
	uint32_t m_evtid1;
	const ppm_param_info* m_arginfo;

	//
	// Note: this copy of the field is used by some fields, like TYPE_ARGS and
	// TYPE_RESARG, that need to do on the fly type customization
	//
	filtercheck_field_info m_customfield;

private:
	int32_t extract_arg(std::string fldname, std::string val, OUT const struct ppm_param_info** parinfo);
	int32_t extract_type(std::string fldname, std::string val, OUT const struct ppm_param_info** parinfo);
	uint8_t* extract_error_count(sinsp_evt *evt, OUT uint32_t* len);
	uint8_t *extract_abspath(sinsp_evt *evt, OUT uint32_t *len);
	inline uint8_t* extract_buflen(sinsp_evt *evt, OUT uint32_t* len);

	bool m_is_compare;
	char* m_storage;
	uint32_t m_storage_size;
	const char* m_cargname;
	sinsp_filter_check_reference* m_converter;
};

//
// user checks
//
class sinsp_filter_check_user : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_UID = 0,
		TYPE_NAME = 1,
		TYPE_HOMEDIR = 2,
		TYPE_SHELL = 3,
		TYPE_LOGINUID = 4,
		TYPE_LOGINNAME = 5,
	};

	sinsp_filter_check_user();
	sinsp_filter_check* allocate_new();
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);

	uint32_t m_uid;
	std::string m_strval;
	int64_t m_s64val;
};

//
// group checks
//
class sinsp_filter_check_group : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_GID,
		TYPE_NAME,
	};

	sinsp_filter_check_group();
	sinsp_filter_check* allocate_new();
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);

	uint32_t m_gid;
	std::string m_name;
};

//
// Tracers
//
#define TEXT_ARG_ID -1000000

class sinsp_filter_check_tracer : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_ID = 0,
		TYPE_TIME,
		TYPE_NTAGS,
		TYPE_NARGS,
		TYPE_TAGS,
		TYPE_TAG,
		TYPE_ARGS,
		TYPE_ARG,
		TYPE_ENTERARGS,
		TYPE_ENTERARG,
		TYPE_DURATION,
		TYPE_DURATION_QUANTIZED,
		TYPE_DURATION_HUMAN,
		TYPE_TAGDURATION,
		TYPE_COUNT,
		TYPE_TAGCOUNT,
		TYPE_TAGCHILDSCOUNT,
		TYPE_IDTAG,
		TYPE_RAWTIME,
		TYPE_RAWPARENTTIME,
	};

	sinsp_filter_check_tracer();
	~sinsp_filter_check_tracer();
	sinsp_filter_check* allocate_new();
	int32_t parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering);
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);

private:
	int32_t extract_arg(std::string fldname, std::string val, OUT const struct ppm_param_info** parinfo);
	inline uint8_t* extract_duration(uint16_t etype, sinsp_tracerparser* eparser, OUT uint32_t* len);
	uint8_t* extract_args(sinsp_partial_tracer* pae, OUT uint32_t *len);
	uint8_t* extract_arg(sinsp_partial_tracer* pae, OUT uint32_t *len);

	int32_t m_argid;
	std::string m_argname;
	const char* m_cargname;
	char* m_storage;
	uint32_t m_storage_size;
	int64_t m_s64val;
	int32_t m_u32val;
	sinsp_filter_check_reference* m_converter;
	std::string m_strstorage;
};

//
// Events in tracers checks
//
class sinsp_filter_check_evtin : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_ID = 0,
		TYPE_NTAGS,
		TYPE_NARGS,
		TYPE_TAGS,
		TYPE_TAG,
		TYPE_ARGS,
		TYPE_ARG,
		TYPE_P_ID,
		TYPE_P_NTAGS,
		TYPE_P_NARGS,
		TYPE_P_TAGS,
		TYPE_P_TAG,
		TYPE_P_ARGS,
		TYPE_P_ARG,
		TYPE_S_ID,
		TYPE_S_NTAGS,
		TYPE_S_NARGS,
		TYPE_S_TAGS,
		TYPE_S_TAG,
		TYPE_S_ARGS,
		TYPE_S_ARG,
		TYPE_M_ID,
		TYPE_M_NTAGS,
		TYPE_M_NARGS,
		TYPE_M_TAGS,
		TYPE_M_TAG,
		TYPE_M_ARGS,
		TYPE_M_ARG,
	};

	sinsp_filter_check_evtin();
	~sinsp_filter_check_evtin();
	int32_t parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering);
	sinsp_filter_check* allocate_new();
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);
	bool compare(sinsp_evt *evt);

	uint64_t m_u64val;
	uint64_t m_tsdelta;
	uint32_t m_u32val;
	std::string m_strstorage;
	std::string m_argname;
	int32_t m_argid;
	uint32_t m_evtid;
	uint32_t m_evtid1;
	const ppm_param_info* m_arginfo;

	//
	// Note: this copy of the field is used by some fields, like TYPE_ARGS and
	// TYPE_RESARG, that need to do on the fly type customization
	//
	filtercheck_field_info m_customfield;

private:
	int32_t extract_arg(std::string fldname, std::string val);
	inline uint8_t* extract_tracer(sinsp_evt *evt, sinsp_partial_tracer* pae, OUT uint32_t* len);
	inline bool compare_tracer(sinsp_evt *evt, sinsp_partial_tracer* pae);

	bool m_is_compare;
	char* m_storage;
	uint32_t m_storage_size;
	const char* m_cargname;
	sinsp_filter_check_reference* m_converter;
};

//
// Fake filter check used by the event formatter to render format text
//
class rawstring_check : public sinsp_filter_check
{
public:
	rawstring_check(std::string text);
	sinsp_filter_check* allocate_new();
	void set_text(std::string text);
	int32_t parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering);
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);

	// XXX this is overkill and wasted for most of the fields.
	// It could be optimized by dynamically allocating the right amount
	// of memory, but we don't care for the moment since we expect filters
	// to be pretty small.
	std::string m_text;
	uint32_t m_text_len;
};

//
// syslog checks
//
class sinsp_decoder_syslog;

class sinsp_filter_check_syslog : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_FACILITY_STR = 0,
		TYPE_FACILITY,
		TYPE_SEVERITY_STR,
		TYPE_SEVERITY,
		TYPE_MESSAGE,
	};

	sinsp_filter_check_syslog();
	sinsp_filter_check* allocate_new();
	int32_t parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering);
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);

	sinsp_decoder_syslog* m_decoder;
	uint32_t m_gid;
	std::string m_name;
};

class sinsp_filter_check_container : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_CONTAINER_ID = 0,
		TYPE_CONTAINER_NAME,
		TYPE_CONTAINER_IMAGE,
		TYPE_CONTAINER_IMAGE_ID,
		TYPE_CONTAINER_TYPE,
		TYPE_CONTAINER_PRIVILEGED,
		TYPE_CONTAINER_MOUNTS,
		TYPE_CONTAINER_MOUNT,
		TYPE_CONTAINER_MOUNT_SOURCE,
		TYPE_CONTAINER_MOUNT_DEST,
		TYPE_CONTAINER_MOUNT_MODE,
		TYPE_CONTAINER_MOUNT_RDWR,
		TYPE_CONTAINER_MOUNT_PROPAGATION,
		TYPE_CONTAINER_IMAGE_REPOSITORY,
		TYPE_CONTAINER_IMAGE_TAG,
		TYPE_CONTAINER_IMAGE_DIGEST,
		TYPE_CONTAINER_HEALTHCHECK,
		TYPE_CONTAINER_LIVENESS_PROBE,
		TYPE_CONTAINER_READINESS_PROBE,
		TYPE_CONTAINER_START_TS,
		TYPE_CONTAINER_DURATION,
		TYPE_CONTAINER_IP_ADDR,
		TYPE_CONTAINER_CNIRESULT,
	};

	sinsp_filter_check_container();
	sinsp_filter_check* allocate_new();
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);

	const std::string &get_argstr();
private:
	int32_t parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering);
	int32_t extract_arg(const std::string& val, size_t basename);

	std::string m_tstr;
	uint32_t m_u32val;
	int32_t m_argid;
	std::string m_argstr;
	int64_t m_s64val;
};

//
// For internal use
//
class sinsp_filter_check_reference : public sinsp_filter_check
{
public:
	enum alignment
	{
		ALIGN_LEFT,
		ALIGN_RIGHT,
	};

	sinsp_filter_check_reference();
	sinsp_filter_check* allocate_new();
	inline void set_val(ppm_param_type type, filtercheck_field_flags flags,
		uint8_t* val, int32_t len,
		uint32_t cnt, ppm_print_format print_format)
	{
		m_finfo.m_type = type;
		m_finfo.m_flags = flags;
		m_val = val;
		m_len = len;
		m_cnt = cnt;
		m_print_format = print_format;
	}
	int32_t parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering);
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);
	char* tostring_nice(sinsp_evt* evt, uint32_t str_len, uint64_t time_delta);
	Json::Value tojson(sinsp_evt* evt, uint32_t str_len, uint64_t time_delta);

private:
	inline char* format_bytes(double val, uint32_t str_len, bool is_int);
	inline char* format_time(uint64_t val, uint32_t str_len);
	char* print_double(uint8_t* rawval, uint32_t str_len);
	char* print_int(uint8_t* rawval, uint32_t str_len);

	filtercheck_field_info m_finfo;
	uint8_t* m_val;
	uint32_t m_len;
	double m_cnt;		// For averages, this stores the entry count
	ppm_print_format m_print_format;
};

//
// For internal use
//
class sinsp_filter_check_utils : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_CNT,
	};

	sinsp_filter_check_utils();
	sinsp_filter_check* allocate_new();
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);

private:
	uint64_t m_cnt;
};

//
// fdlist checks
//
class sinsp_filter_check_fdlist : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_FDNUMS = 0,
		TYPE_FDNAMES = 1,
		TYPE_CLIENTIPS = 2,
		TYPE_SERVERIPS = 3,
		TYPE_CLIENTPORTS = 4,
		TYPE_SERVERPORTS = 5,
	};

	sinsp_filter_check_fdlist();
	sinsp_filter_check* allocate_new();
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);

private:
	std::string m_strval;
	char m_addrbuff[100];
};

#if !defined(CYGWING_AGENT) && !defined(MINIMAL_BUILD) && !defined(__EMSCRIPTEN__)

class sinsp_filter_check_k8s : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_K8S_POD_NAME = 0,
		TYPE_K8S_POD_ID,
		TYPE_K8S_POD_LABEL,
		TYPE_K8S_POD_LABELS,
		TYPE_K8S_POD_IP,
		TYPE_K8S_POD_CNIRESULT,
		TYPE_K8S_RC_NAME,
		TYPE_K8S_RC_ID,
		TYPE_K8S_RC_LABEL,
		TYPE_K8S_RC_LABELS,
		TYPE_K8S_SVC_NAME,
		TYPE_K8S_SVC_ID,
		TYPE_K8S_SVC_LABEL,
		TYPE_K8S_SVC_LABELS,
		TYPE_K8S_NS_NAME,
		TYPE_K8S_NS_ID,
		TYPE_K8S_NS_LABEL,
		TYPE_K8S_NS_LABELS,
		TYPE_K8S_RS_NAME,
		TYPE_K8S_RS_ID,
		TYPE_K8S_RS_LABEL,
		TYPE_K8S_RS_LABELS,
		TYPE_K8S_DEPLOYMENT_NAME,
		TYPE_K8S_DEPLOYMENT_ID,
		TYPE_K8S_DEPLOYMENT_LABEL,
		TYPE_K8S_DEPLOYMENT_LABELS,
	};

	sinsp_filter_check_k8s();
	sinsp_filter_check* allocate_new();
	int32_t parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering);
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);

private:
	int32_t extract_arg(const std::string& fldname, const std::string& val);
	const k8s_pod_t* find_pod_for_thread(const sinsp_threadinfo* tinfo);
	const k8s_ns_t* find_ns_by_name(const std::string& ns_name);
	const k8s_rc_t* find_rc_by_pod(const k8s_pod_t* pod);
	const k8s_rs_t* find_rs_by_pod(const k8s_pod_t* pod);
	std::vector<const k8s_service_t*> find_svc_by_pod(const k8s_pod_t* pod);
	const k8s_deployment_t* find_deployment_by_pod(const k8s_pod_t* pod);
	void concatenate_labels(const k8s_pair_list& labels, std::string* s);
	void concatenate_container_labels(const std::map<std::string, std::string>& labels, std::string* s);
	bool find_label(const k8s_pair_list& labels, const std::string& key, std::string* value);
	std::string m_argname;
	std::string m_tstr;
	uint32_t m_u32val;
};

class sinsp_filter_check_mesos : public sinsp_filter_check
{
public:
	enum check_type
	{
		TYPE_MESOS_TASK_NAME = 0,
		TYPE_MESOS_TASK_ID,
		TYPE_MESOS_TASK_LABEL,
		TYPE_MESOS_TASK_LABELS,
		TYPE_MESOS_FRAMEWORK_NAME,
		TYPE_MESOS_FRAMEWORK_ID,
		TYPE_MARATHON_APP_NAME,
		TYPE_MARATHON_APP_ID,
		TYPE_MARATHON_APP_LABEL,
		TYPE_MARATHON_APP_LABELS,
		TYPE_MARATHON_GROUP_NAME,
		TYPE_MARATHON_GROUP_ID,
	};

	sinsp_filter_check_mesos();
	sinsp_filter_check* allocate_new();
	int32_t parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering);
	uint8_t* extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings = true);

private:

	int32_t extract_arg(const std::string& fldname, const std::string& val);
	mesos_task::ptr_t find_task_for_thread(const sinsp_threadinfo* tinfo);
	const mesos_framework* find_framework_by_task(mesos_task::ptr_t task);
	marathon_app::ptr_t find_app_by_task(mesos_task::ptr_t task);
	marathon_group::ptr_t find_group_by_task(mesos_task::ptr_t task);
	void concatenate_labels(const mesos_pair_list& labels, std::string* s);
	bool find_label(const mesos_pair_list& labels, const std::string& key, std::string* value);

	std::string m_argname;
	std::string m_tstr;
};

#endif // !defined(CYGWING_AGENT) && !defined(MINIMAL_BUILD)
