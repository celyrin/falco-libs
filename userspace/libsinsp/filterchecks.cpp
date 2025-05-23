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

#include <time.h>
#include <math.h>
#ifndef _WIN32
#include <algorithm>
#endif
#include <filesystem>
#include "sinsp.h"
#include "sinsp_int.h"
#include "dns_manager.h"

#include "filter.h"
#include "filterchecks.h"
#include "plugin.h"
#include "plugin_manager.h"
#include "protodecoder.h"
#include "tracers.h"
#include "value_parser.h"

#include "strl.h"

using namespace std;

extern sinsp_evttables g_infotables;
int32_t g_screen_w = -1;
bool g_filterchecks_force_raw_times = false;

#define RETURN_EXTRACT_VAR(x) do {  \
        *len = sizeof((x));         \
        return (uint8_t*) &(x);     \
} while(0)

#define RETURN_EXTRACT_PTR(x) do {  \
        *len = sizeof(*(x));        \
        return (uint8_t*) (x);      \
} while(0)

#define RETURN_EXTRACT_STRING(x) do {  \
        *len = (x).size();             \
        return (uint8_t*) (x).c_str(); \
} while(0)

#define RETURN_EXTRACT_CSTR(x) do {             \
        if((x))                                 \
        {                                       \
                *len = strlen((char *) ((x)));  \
        }                                       \
        return (uint8_t*) ((x));                \
} while(0)

///////////////////////////////////////////////////////////////////////////////
// Helper functions
///////////////////////////////////////////////////////////////////////////////
int32_t gmt2local(time_t t)
{
	int dt, dir;
	struct tm *gmt, *tmp_gmt, *loc;
	struct tm sgmt;

	if(t == 0)
	{
		t = time(NULL);
	}

	gmt = &sgmt;
	tmp_gmt = gmtime(&t);
	if (tmp_gmt == NULL)
	{
		throw sinsp_exception("cannot get gmtime");
	}
	*gmt = *tmp_gmt;
	loc = localtime(&t);
	if(loc == NULL)
	{
		throw sinsp_exception("cannot get localtime");
	}

	dt = (loc->tm_hour - gmt->tm_hour) * 60 * 60 + (loc->tm_min - gmt->tm_min) * 60;

	dir = loc->tm_year - gmt->tm_year;
	if(dir == 0)
	{
		dir = loc->tm_yday - gmt->tm_yday;
	}

	dt += dir * 24 * 60 * 60;

	return dt;
}

static inline bool str_match_start(const std::string& val, size_t len, const char* m)
{
	return val.compare(0, len, m) == 0;
}

#define STR_MATCH(s) str_match_start(val, sizeof (s) -1, s)
///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_fspath implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_fspath_fields[] =
	{
		{PT_CHARBUF, EPF_NONE, PF_NA, "fs.path.name", "Path for Filesystem-related operation", "For any event type that deals with a filesystem path, the path the file syscall is operating on. This path is always fully resolved, prepending the thread cwd when needed."},
		{PT_CHARBUF, EPF_NONE, PF_NA, "fs.path.nameraw", "Raw path for Filesystem-related operation", "For any event type that deals with a filesystem path, the path the file syscall is operating on. This path is always the path provided to the syscall and may not be fully resolved."},
		{PT_CHARBUF, EPF_NONE, PF_NA, "fs.path.source", "Source path for Filesystem-related operation", "For any event type that deals with a filesystem path, and specifically for a source and target like mv, cp, etc, the source path the file syscall is operating on. This path is always fully resolved, prepending the thread cwd when needed."},
		{PT_CHARBUF, EPF_NONE, PF_NA, "fs.path.sourceraw", "Source path for Filesystem-related operation", "For any event type that deals with a filesystem path, and specifically for a source and target like mv, cp, etc, the source path the file syscall is operating on. This path is always the path provided to the syscall and may not be fully resolved."},
		{PT_CHARBUF, EPF_NONE, PF_NA, "fs.path.target", "Target path for Filesystem-related operation", "For any event type that deals with a filesystem path, and specifically for a target and target like mv, cp, etc, the target path the file syscall is operating on. This path is always fully resolved, prepending the thread cwd when needed."},
		{PT_CHARBUF, EPF_NONE, PF_NA, "fs.path.targetraw", "Target path for Filesystem-related operation", "For any event type that deals with a filesystem path, and specifically for a target and target like mv, cp, etc, the target path the file syscall is operating on. This path is always the path provided to the syscall and may not be fully resolved."},
};

sinsp_filter_check_fspath::sinsp_filter_check_fspath()
	// These will either be populated when calling
	// create_fspath_checks or copied from another filtercheck
	// when calling set_fspath_checks().
	: m_success_checks(new filtercheck_map_t()),
	  m_path_checks(new filtercheck_map_t()),
	  m_source_checks(new filtercheck_map_t()),
	  m_target_checks(new filtercheck_map_t())
{
	m_info.m_name = "fs.path";
	m_info.m_desc = "Every syscall that has a filesystem path in its arguments has these fields set with information related to the path arguments. This differs from the fd.* fields as it includes syscalls like unlink, rename, etc. that act directly on filesystem paths as compared to opened file descriptors.";
	m_info.m_fields = sinsp_filter_check_fspath_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_fspath_fields) / sizeof(sinsp_filter_check_fspath_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;
};

std::shared_ptr<sinsp_filter_check> sinsp_filter_check_fspath::create_event_check(const char *name,
										  cmpop cop,
										  const char *value)
{
	std::shared_ptr<sinsp_filter_check> chk(new sinsp_filter_check_event());

	chk->m_inspector = m_inspector;
	chk->m_cmpop = cop;
	chk->m_boolop = BO_NONE;
	chk->parse_field_name(name, true, true);

	if(value)
	{
		chk->add_filter_value(value, strlen(value), 0);
	}

	return chk;
}

std::shared_ptr<sinsp_filter_check> sinsp_filter_check_fspath::create_fd_check(const char *name)
{
	std::shared_ptr<sinsp_filter_check> chk(new sinsp_filter_check_fd());

	chk->m_inspector = m_inspector;
	chk->m_cmpop = CO_NONE;
	chk->m_boolop = BO_NONE;
	chk->parse_field_name(name, true, true);

	return chk;
}

void sinsp_filter_check_fspath::create_fspath_checks()
{
	std::shared_ptr<sinsp_filter_check> evt_arg_path = create_event_check("evt.rawarg.path");
	std::shared_ptr<sinsp_filter_check> evt_arg_pathname = create_event_check("evt.rawarg.pathname");
	std::shared_ptr<sinsp_filter_check> evt_arg_res_eq_0 = create_event_check("evt.rawarg.res", CO_EQ, "0");
	std::shared_ptr<sinsp_filter_check> evt_arg_name = create_event_check("evt.rawarg.name");
	std::shared_ptr<sinsp_filter_check> evt_fd_name = create_fd_check("fd.name");
	std::shared_ptr<sinsp_filter_check> evt_arg_fd_ne_neg1 = create_event_check("evt.rawarg.fd", CO_NE, "-1");
	std::shared_ptr<sinsp_filter_check> evt_arg_oldpath = create_event_check("evt.rawarg.oldpath");
	std::shared_ptr<sinsp_filter_check> evt_arg_newpath = create_event_check("evt.rawarg.newpath");
	std::shared_ptr<sinsp_filter_check> evt_arg_linkpath = create_event_check("evt.rawarg.linkpath");
	std::shared_ptr<sinsp_filter_check> evt_arg_target = create_event_check("evt.rawarg.target");
	std::shared_ptr<sinsp_filter_check> evt_arg_filename = create_event_check("evt.rawarg.filename");
	std::shared_ptr<sinsp_filter_check> evt_arg_special = create_event_check("evt.rawarg.special");
	std::shared_ptr<sinsp_filter_check> evt_arg_dev = create_event_check("evt.rawarg.dev");
	std::shared_ptr<sinsp_filter_check> evt_arg_dir = create_event_check("evt.rawarg.dir");

	m_success_checks->emplace(PPME_SYSCALL_MKDIR_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_MKDIR_2_X, evt_arg_path);
	m_success_checks->emplace(PPME_SYSCALL_MKDIR_2_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_MKDIRAT_X, evt_arg_path);
	m_success_checks->emplace(PPME_SYSCALL_MKDIRAT_X, evt_arg_res_eq_0);

	m_success_checks->emplace(PPME_SYSCALL_RMDIR_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_RMDIR_2_X, evt_arg_path);
	m_success_checks->emplace(PPME_SYSCALL_RMDIR_2_X, evt_arg_res_eq_0);

	m_success_checks->emplace(PPME_SYSCALL_UNLINK_X, evt_arg_res_eq_0);

	m_success_checks->emplace(PPME_SYSCALL_UNLINKAT_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_UNLINK_2_X, evt_arg_path);
	m_success_checks->emplace(PPME_SYSCALL_UNLINK_2_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_UNLINKAT_2_X, evt_arg_name);
	m_success_checks->emplace(PPME_SYSCALL_UNLINKAT_2_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_OPEN_X, evt_arg_name);
	m_success_checks->emplace(PPME_SYSCALL_OPEN_X, evt_arg_fd_ne_neg1);

	m_success_checks->emplace(PPME_SYSCALL_OPENAT_X, evt_arg_fd_ne_neg1);

	m_path_checks->emplace(PPME_SYSCALL_OPENAT_2_X, evt_arg_name);
	m_success_checks->emplace(PPME_SYSCALL_OPENAT_2_X, evt_arg_fd_ne_neg1);

	m_path_checks->emplace(PPME_SYSCALL_OPENAT2_X, evt_arg_name);
	m_success_checks->emplace(PPME_SYSCALL_OPENAT2_X, evt_arg_fd_ne_neg1);

	m_path_checks->emplace(PPME_SYSCALL_FCHMODAT_X, evt_arg_filename);
	m_success_checks->emplace(PPME_SYSCALL_FCHMODAT_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_CHMOD_X, evt_arg_filename);
	m_success_checks->emplace(PPME_SYSCALL_CHMOD_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_FCHMOD_X, evt_fd_name);
	m_success_checks->emplace(PPME_SYSCALL_FCHMOD_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_CHOWN_X, evt_arg_path);
	m_success_checks->emplace(PPME_SYSCALL_CHOWN_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_LCHOWN_X, evt_arg_path);
	m_success_checks->emplace(PPME_SYSCALL_LCHOWN_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_FCHOWN_X, evt_fd_name);
	m_success_checks->emplace(PPME_SYSCALL_FCHOWN_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_FCHOWNAT_X, evt_arg_pathname);
	m_success_checks->emplace(PPME_SYSCALL_FCHOWNAT_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_QUOTACTL_X, evt_arg_special);
	m_success_checks->emplace(PPME_SYSCALL_QUOTACTL_X, evt_arg_res_eq_0);

	m_source_checks->emplace(PPME_SYSCALL_RENAME_X, evt_arg_oldpath);
	m_target_checks->emplace(PPME_SYSCALL_RENAME_X, evt_arg_newpath);
	m_success_checks->emplace(PPME_SYSCALL_RENAME_X, evt_arg_res_eq_0);

	m_source_checks->emplace(PPME_SYSCALL_RENAMEAT_X, evt_arg_oldpath);
	m_target_checks->emplace(PPME_SYSCALL_RENAMEAT_X, evt_arg_newpath);
	m_success_checks->emplace(PPME_SYSCALL_RENAMEAT_X, evt_arg_res_eq_0);

	m_source_checks->emplace(PPME_SYSCALL_RENAMEAT2_X, evt_arg_oldpath);
	m_target_checks->emplace(PPME_SYSCALL_RENAMEAT2_X, evt_arg_newpath);
	m_success_checks->emplace(PPME_SYSCALL_RENAMEAT2_X, evt_arg_res_eq_0);

	m_success_checks->emplace(PPME_SYSCALL_LINK_X, evt_arg_res_eq_0);

	m_success_checks->emplace(PPME_SYSCALL_LINKAT_X, evt_arg_res_eq_0);

	m_source_checks->emplace(PPME_SYSCALL_LINK_2_X, evt_arg_newpath);
	m_target_checks->emplace(PPME_SYSCALL_LINK_2_X, evt_arg_oldpath);
	m_success_checks->emplace(PPME_SYSCALL_LINK_2_X, evt_arg_res_eq_0);

	m_source_checks->emplace(PPME_SYSCALL_LINKAT_2_X, evt_arg_newpath);
	m_target_checks->emplace(PPME_SYSCALL_LINKAT_2_X, evt_arg_oldpath);
	m_success_checks->emplace(PPME_SYSCALL_LINKAT_2_X, evt_arg_res_eq_0);

	m_source_checks->emplace(PPME_SYSCALL_SYMLINK_X, evt_arg_linkpath);
	m_target_checks->emplace(PPME_SYSCALL_SYMLINK_X, evt_arg_target);
	m_success_checks->emplace(PPME_SYSCALL_SYMLINK_X, evt_arg_res_eq_0);

	m_source_checks->emplace(PPME_SYSCALL_SYMLINKAT_X, evt_arg_linkpath);
	m_target_checks->emplace(PPME_SYSCALL_SYMLINKAT_X, evt_arg_target);
	m_success_checks->emplace(PPME_SYSCALL_SYMLINKAT_X, evt_arg_res_eq_0);

	m_source_checks->emplace(PPME_SYSCALL_MOUNT_X, evt_arg_dev);
	m_target_checks->emplace(PPME_SYSCALL_MOUNT_X, evt_arg_dir);
	m_success_checks->emplace(PPME_SYSCALL_MOUNT_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_UMOUNT_X, evt_arg_name);
	m_success_checks->emplace(PPME_SYSCALL_UMOUNT_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_UMOUNT_1_X, evt_arg_name);
	m_success_checks->emplace(PPME_SYSCALL_UMOUNT_1_X, evt_arg_res_eq_0);

	m_path_checks->emplace(PPME_SYSCALL_UMOUNT2_X, evt_arg_name);
	m_success_checks->emplace(PPME_SYSCALL_UMOUNT2_X, evt_arg_res_eq_0);
}

void sinsp_filter_check_fspath::set_fspath_checks(std::shared_ptr<filtercheck_map_t> success_checks,
						  std::shared_ptr<filtercheck_map_t> path_checks,
						  std::shared_ptr<filtercheck_map_t> source_checks,
						  std::shared_ptr<filtercheck_map_t> target_checks)
{
	m_success_checks = success_checks;
	m_path_checks = path_checks;
	m_source_checks = source_checks;
	m_target_checks = target_checks;
}

sinsp_filter_check* sinsp_filter_check_fspath::allocate_new()
{
	// If not yet populated, do so now. The maps will be empty
	// *only* for the initial filtercheck created in
	// filter_check_list.
	if(m_path_checks->empty())
	{
		create_fspath_checks();
	}

	sinsp_filter_check_fspath* ret = new sinsp_filter_check_fspath();

	ret->set_fspath_checks(m_success_checks, m_path_checks, m_source_checks, m_target_checks);

	return ret;
}

uint8_t* sinsp_filter_check_fspath::extract(sinsp_evt* evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	ASSERT(evt);

	// First check the success conditions.
	auto it = m_success_checks->find(evt->get_type());
	if(it == m_success_checks->end() || !it->second->compare(evt))
	{
		return NULL;
	}

	std::optional<std::reference_wrapper<std::string>> enter_param;
	std::vector<extract_value_t> extract_values;

	switch(m_field_id)
	{
	case TYPE_NAME:
	case TYPE_NAMERAW:

		// For some event types we need to get the values from the enter event instead.
		switch(evt->get_type())
		{
		case PPME_SYSCALL_MKDIR_X:
		case PPME_SYSCALL_RMDIR_X:
		case PPME_SYSCALL_UNLINK_X:
			enter_param = evt->get_enter_evt_param("path");
			if(!enter_param.has_value())
			{
				return NULL;
			}
			m_tstr = enter_param.value();
			break;
		case PPME_SYSCALL_UNLINKAT_X:
		case PPME_SYSCALL_OPENAT_X:
			enter_param = evt->get_enter_evt_param("name");
			if(!enter_param.has_value())
			{
				return NULL;
			}
			m_tstr = enter_param.value();
			break;
		default:
			if (!extract_fspath(evt, extract_values, m_path_checks))
			{
				return NULL;
			}
			m_tstr.assign((char *) extract_values[0].ptr, extract_values[0].len);
		};

		break;
	case TYPE_SOURCE:
	case TYPE_SOURCERAW:
		// For some event types we need to get the values from the enter event instead.
		switch(evt->get_type())
		{
		case PPME_SYSCALL_LINK_X:
		case PPME_SYSCALL_LINKAT_X:
			enter_param = evt->get_enter_evt_param("newpath");
			if(!enter_param.has_value())
			{
				return NULL;
			}
			m_tstr = enter_param.value();
			break;
		default:
			if(!extract_fspath(evt, extract_values, m_source_checks))
			{
				return NULL;
			}
			m_tstr.assign((char *) extract_values[0].ptr, extract_values[0].len);
		};
		break;
	case TYPE_TARGET:
	case TYPE_TARGETRAW:

		// For some event types we need to get the values from the enter event instead.
		switch(evt->get_type())
		{
		case PPME_SYSCALL_LINK_X:
		case PPME_SYSCALL_LINKAT_X:
			enter_param = evt->get_enter_evt_param("oldpath");
			if(!enter_param.has_value())
			{
				return NULL;
			}
			m_tstr = enter_param.value();
			break;
		default:
			if (!extract_fspath(evt, extract_values, m_target_checks))
			{
				return NULL;
			}
			m_tstr.assign((char *) extract_values[0].ptr, extract_values[0].len);
		};
		break;
	default:
		return NULL;
	}

	// For the non-raw fields, if the path is not absolute,
	// prepend the cwd of the threadinfo to the path.
	if((m_field_id == TYPE_NAME ||
	    m_field_id == TYPE_SOURCE ||
	    m_field_id == TYPE_TARGET)
	   && m_tstr.front() != '/')
	{
		sinsp_threadinfo* tinfo = evt->get_thread_info();

		std::filesystem::path tstr = tinfo->get_cwd() + m_tstr;
		m_tstr = std::filesystem::absolute(tstr).lexically_normal().string();
	}

	// If m_tstr ends in a c-style \0, remove it to be
	// consistent. Generally, the evt.rawarg fields above *will*
	// end in c-style \0 characters, while the ones that extract
	// from the enter event or have values populated by parsers
	// (e.g. fchmod/fchown) will not.
	if(m_tstr.back() == '\0')
	{
		m_tstr.pop_back();
	}

	RETURN_EXTRACT_STRING(m_tstr);
}



bool sinsp_filter_check_fspath::extract_fspath(sinsp_evt* evt,
					       OUT std::vector<extract_value_t>& values,
					       std::shared_ptr<filtercheck_map_t> checks)
{
	sinsp_evt* extract_evt = evt;

	auto it = checks->find(extract_evt->get_type());
	if(it == checks->end())
	{
		return false;
	}

	if(!it->second->extract(extract_evt, values, true) ||
	   values.size() != 1)
	{
		return false;
	}

	return true;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_fd implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_fd_fields[] =
{
	{PT_INT64, EPF_NONE, PF_ID, "fd.num", "FD Number", "the unique number identifying the file descriptor."},
	{PT_CHARBUF, EPF_NONE, PF_DEC, "fd.type", "FD Type", "type of FD. Can be 'file', 'directory', 'ipv4', 'ipv6', 'unix', 'pipe', 'event', 'signalfd', 'eventpoll', 'inotify'  'signalfd' or 'memfd'."},
	{PT_CHARBUF, EPF_NONE, PF_DEC, "fd.typechar", "FD Type Char", "type of FD as a single character. Can be 'f' for file, 4 for IPv4 socket, 6 for IPv6 socket, 'u' for unix socket, p for pipe, 'e' for eventfd, 's' for signalfd, 'l' for eventpoll, 'i' for inotify, 'b' for bpf, 'u' for userfaultd, 'r' for io_uring, 'm' for memfd ,'o' for unknown."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.name", "FD Name", "FD full name. If the fd is a file, this field contains the full path. If the FD is a socket, this field contain the connection tuple."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.directory", "FD Directory", "If the fd is a file, the directory that contains it."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.filename", "FD Filename", "If the fd is a file, the filename without the path."},
	{PT_IPADDR, EPF_FILTER_ONLY, PF_NA, "fd.ip", "FD IP Address", "matches the ip address (client or server) of the fd."},
	{PT_IPADDR, EPF_NONE, PF_NA, "fd.cip", "FD Client Address", "client IP address."},
	{PT_IPADDR, EPF_NONE, PF_NA, "fd.sip", "FD Server Address", "server IP address."},
	{PT_IPADDR, EPF_NONE, PF_NA, "fd.lip", "FD Local Address", "local IP address."},
	{PT_IPADDR, EPF_NONE, PF_NA, "fd.rip", "FD Remote Address", "remote IP address."},
	{PT_PORT, EPF_FILTER_ONLY, PF_DEC, "fd.port", "FD Port", "matches the port (either client or server) of the fd."},
	{PT_PORT, EPF_NONE, PF_DEC, "fd.cport", "FD Client Port", "for TCP/UDP FDs, the client port."},
	{PT_PORT, EPF_NONE, PF_DEC, "fd.sport", "FD Server Port", "for TCP/UDP FDs, server port."},
	{PT_PORT, EPF_NONE, PF_DEC, "fd.lport", "FD Local Port", "for TCP/UDP FDs, the local port."},
	{PT_PORT, EPF_NONE, PF_DEC, "fd.rport", "FD Remote Port", "for TCP/UDP FDs, the remote port."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.l4proto", "FD IP Protocol", "the IP protocol of a socket. Can be 'tcp', 'udp', 'icmp' or 'raw'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.sockfamily", "FD Socket Family", "the socket family for socket events. Can be 'ip' or 'unix'."},
	{PT_BOOL, EPF_NONE, PF_NA, "fd.is_server", "FD Server", "'true' if the process owning this FD is the server endpoint in the connection."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.uid", "FD ID", "a unique identifier for the FD, created by chaining the FD number and the thread ID."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.containername", "FD Container Name", "chaining of the container ID and the FD name. Useful when trying to identify which container an FD belongs to."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.containerdirectory", "FD Container Directory", "chaining of the container ID and the directory name. Useful when trying to identify which container a directory belongs to."},
	{PT_PORT, EPF_FILTER_ONLY, PF_NA, "fd.proto", "FD Protocol", "matches the protocol (either client or server) of the fd."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.cproto", "FD Client Protocol", "for TCP/UDP FDs, the client protocol."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.sproto", "FD Server Protocol", "for TCP/UDP FDs, server protocol."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.lproto", "FD Local Protocol", "for TCP/UDP FDs, the local protocol."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.rproto", "FD Remote Protocol", "for TCP/UDP FDs, the remote protocol."},
	{PT_IPNET, EPF_FILTER_ONLY, PF_NA, "fd.net", "FD IP Network", "matches the IP network (client or server) of the fd."},
	{PT_IPNET, EPF_FILTER_ONLY, PF_NA, "fd.cnet", "FD Client Network", "matches the client IP network of the fd."},
	{PT_IPNET, EPF_FILTER_ONLY, PF_NA, "fd.snet", "FD Server Network", "matches the server IP network of the fd."},
	{PT_IPNET, EPF_FILTER_ONLY, PF_NA, "fd.lnet", "FD Local Network", "matches the local IP network of the fd."},
	{PT_IPNET, EPF_FILTER_ONLY, PF_NA, "fd.rnet", "FD Remote Network", "matches the remote IP network of the fd."},
	{PT_BOOL, EPF_NONE, PF_NA, "fd.connected", "FD Connected", "for TCP/UDP FDs, 'true' if the socket is connected."},
	{PT_BOOL, EPF_NONE, PF_NA, "fd.name_changed", "FD Name Changed", "True when an event changes the name of an fd used by this event. This can occur in some cases such as udp connections where the connection tuple changes."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.cip.name", "FD Client Domain Name", "Domain name associated with the client IP address."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.sip.name", "FD Server Domain Name", "Domain name associated with the server IP address."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.lip.name", "FD Local Domain Name", "Domain name associated with the local IP address."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.rip.name", "FD Remote Domain Name", "Domain name associated with the remote IP address."},
	{PT_INT32, EPF_NONE, PF_HEX, "fd.dev", "FD Device", "device number (major/minor) containing the referenced file"},
	{PT_INT32, EPF_NONE, PF_DEC, "fd.dev.major", "FD Major Device", "major device number containing the referenced file"},
	{PT_INT32, EPF_NONE, PF_DEC, "fd.dev.minor", "FD Minor Device", "minor device number containing the referenced file"},
	{PT_INT64, EPF_NONE, PF_DEC, "fd.ino", "FD Inode Number", "inode number of the referenced file"},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fd.nameraw", "FD Name Raw", "FD full name raw. Just like fd.name, but only used if fd is a file path. File path is kept raw with limited sanitization and without deriving the absolute path."},
	{PT_CHARBUF, EPF_IS_LIST, PF_DEC, "fd.types", "FD Type", "List of FD types in used. Can be passed an fd number e.g. fd.types[0] to get the type of stdout as a single item list."},
};

sinsp_filter_check_fd::sinsp_filter_check_fd()
{
	m_tinfo = NULL;
	m_fdinfo = NULL;
	m_argid = -1;

	m_info.m_name = "fd";
	m_info.m_desc = "Every syscall that has a file descriptor in its arguments has these fields set with information related to the file.";
	m_info.m_fields = sinsp_filter_check_fd_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_fd_fields) / sizeof(sinsp_filter_check_fd_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;
}

sinsp_filter_check* sinsp_filter_check_fd::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_fd();
}

int32_t sinsp_filter_check_fd::extract_arg(string fldname, string val)
{
	uint32_t parsed_len = 0;

	//
	// 'arg' and 'resarg' are handled in a custom way
	//
	if(val[fldname.size()] == '[')
	{
		parsed_len = (uint32_t)val.find(']');
		string numstr = val.substr(fldname.size() + 1, parsed_len - fldname.size() - 1);

		m_argid = sinsp_numparser::parsed64(numstr);

		parsed_len++;
	}

	return parsed_len;
}


int32_t sinsp_filter_check_fd::parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering)
{
	string val(str);

	if(STR_MATCH("fd.types"))
	{
		m_field_id = TYPE_FDTYPES;
		m_field = &m_info.m_fields[m_field_id];
		int32_t res = 0;

		res = extract_arg("fd.types", val);

		if(res == 0)
		{
			m_argid = -1;
			res = (int32_t)val.size();
		}

		return res;
	}

	return sinsp_filter_check::parse_field_name(str, alloc_state, needed_for_filtering);
}

bool sinsp_filter_check_fd::extract_fdname_from_creator(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings, bool fd_nameraw)
{
	const char* resolved_argstr;
	uint16_t etype = evt->get_type();

	if(PPME_IS_ENTER(etype))
	{
		return false;
	}

	switch(etype)
	{
	case PPME_SYSCALL_OPEN_X:
	case PPME_SOCKET_ACCEPT_X:
	case PPME_SOCKET_ACCEPT_5_X:
	case PPME_SOCKET_ACCEPT4_X:
	case PPME_SOCKET_ACCEPT4_5_X:
	case PPME_SOCKET_ACCEPT4_6_X:
	case PPME_SYSCALL_CREAT_X:
		{
			const char* argstr = evt->get_param_as_str(1, &resolved_argstr,
				m_inspector->get_buffer_format());

			if(resolved_argstr[0] != 0)
			{
				m_tstr = resolved_argstr;
			}
			else
			{
				m_tstr = argstr;
			}

			return true;
		}
	case PPME_SOCKET_CONNECT_X:
		{
			const char* argstr = evt->get_param_as_str(1, &resolved_argstr,
				m_inspector->get_buffer_format());

			if(resolved_argstr[0] != 0)
			{
				m_tstr = resolved_argstr;
			}
			else
			{
				m_tstr = argstr;
			}

			return true;
		}
	case PPME_SYSCALL_OPENAT_X:
	case PPME_SYSCALL_OPENAT_2_X:
	case PPME_SYSCALL_OPENAT2_X:
		{
			sinsp_evt enter_evt;
			sinsp_evt_param *parinfo;
			char *name;
			uint32_t namelen;
			string sdir;

			if(etype == PPME_SYSCALL_OPENAT_X)
			{
				//
				// XXX This is highly inefficient, as it re-requests the enter event and then
				// does unnecessary allocations and copies. We assume that failed openat() happen
				// rarely enough that we don't care.
				//
				if(!m_inspector->get_parser()->retrieve_enter_event(&enter_evt, evt))
				{
					return false;
				}
			}

			parinfo = etype == PPME_SYSCALL_OPENAT_X ? enter_evt.get_param(1) : evt->get_param(2);
			name = parinfo->m_val;
			namelen = parinfo->m_len;

			parinfo = etype == PPME_SYSCALL_OPENAT_X ? enter_evt.get_param(0) : evt->get_param(1);
			ASSERT(parinfo->m_len == sizeof(int64_t));
			int64_t dirfd = *(int64_t *)parinfo->m_val;

			sinsp_parser::parse_dirfd(evt, name, dirfd, &sdir);

			char fullpath[SCAP_MAX_PATH_SIZE];

			sinsp_utils::concatenate_paths(fullpath, SCAP_MAX_PATH_SIZE,
				sdir.c_str(),
				(uint32_t)sdir.length(),
				name,
				namelen);

			if(fd_nameraw)
			{
				m_tstr = name;
			}
			else
			{
				m_tstr = fullpath;
			}

			if(sanitize_strings)
			{
				sanitize_string(m_tstr);
			}

			return true;
		}
	case PPME_SYSCALL_OPEN_BY_HANDLE_AT_X:
		{
			sinsp_evt_param *parinfo;
			char *fullpath;

			parinfo = evt->get_param(3);
			fullpath = parinfo->m_val;
			m_tstr = fullpath;

			if(sanitize_strings)
			{
				sanitize_string(m_tstr);
			}

			return true;
		}
	default:
		m_tstr = "";
		return true;
	}
}

uint8_t* sinsp_filter_check_fd::extract_from_null_fd(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	//
	// Even is there's no fd, we still try to extract a name from exit events that create
	// one. With these events, the fact that there's no FD means that the call failed,
	// but even if that happened we still want to collect the name.
	//
	switch(m_field_id)
	{
	case TYPE_FDNAME:
	{
		if(extract_fdname_from_creator(evt, len, sanitize_strings) == true)
		{
			RETURN_EXTRACT_STRING(m_tstr);
		}
		else
		{
			return NULL;
		}
	}
	case TYPE_CONTAINERNAME:
	{
		if(extract_fdname_from_creator(evt, len, sanitize_strings) == true)
		{
			m_tstr = m_tinfo->m_container_id + ':' + m_tstr;
			RETURN_EXTRACT_STRING(m_tstr);
		}
		else
		{
			return NULL;
		}
	}
	case TYPE_DIRECTORY:
	case TYPE_CONTAINERDIRECTORY:
	{
		if(extract_fdname_from_creator(evt, len, sanitize_strings) == true)
		{
			if(sanitize_strings)
			{
				sanitize_string(m_tstr);
			}

			size_t pos = m_tstr.rfind('/');
			if(pos != string::npos && pos != 0)
			{
				if(pos < m_tstr.size() - 1)
				{
					m_tstr.resize(pos);
				}
			}
			else
			{
				m_tstr = "/";
			}

			if(m_field_id == TYPE_CONTAINERDIRECTORY)
			{
				m_tstr = m_tinfo->m_container_id + ':' + m_tstr;
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
		else
		{
			return NULL;
		}
	}
	case TYPE_FILENAME:
	{
		if(evt->get_type() != PPME_SYSCALL_OPEN_E && evt->get_type() != PPME_SYSCALL_OPENAT_E &&
			evt->get_type() != PPME_SYSCALL_OPENAT_2_E && evt->get_type() != PPME_SYSCALL_OPENAT2_E &&
			evt->get_type() != PPME_SYSCALL_CREAT_E)
		{
			return NULL;
		}

		if(extract_fdname_from_creator(evt, len, sanitize_strings) == true)
		{
			if(sanitize_strings)
			{
				sanitize_string(m_tstr);
			}

			size_t pos = m_tstr.rfind('/');
			if(pos != string::npos)
			{
				if(pos < m_tstr.size() - 1)
				{
					m_tstr = m_tstr.substr(pos + 1, string::npos);
				}
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
		else
		{
			return NULL;
		}
	}
	case TYPE_FDTYPECHAR:
		*len = 1;
		switch(PPME_MAKE_ENTER(evt->get_type()))
		{
		case PPME_SYSCALL_OPEN_E:
		case PPME_SYSCALL_OPENAT_E:
		case PPME_SYSCALL_OPENAT_2_E:
		case PPME_SYSCALL_OPENAT2_E:
		case PPME_SYSCALL_CREAT_E:
			m_tcstr[0] = CHAR_FD_FILE;
			m_tcstr[1] = 0;
			return m_tcstr;
		case PPME_SOCKET_SOCKET_E:
		case PPME_SOCKET_ACCEPT_E:
		case PPME_SOCKET_ACCEPT_5_E:
		case PPME_SOCKET_ACCEPT4_E:
		case PPME_SOCKET_ACCEPT4_5_E:
		case PPME_SOCKET_ACCEPT4_6_E:
                	//
                	// Note, this is not accurate, because it always
                	// returns IPv4 even if this could be IPv6 or unix.
                	// For the moment, I assume it's better than nothing, and doing
                	// real event parsing here would be a pain.
                	//
                	m_tcstr[0] = CHAR_FD_IPV4_SOCK;
                	m_tcstr[1] = 0;
                	return m_tcstr;
		case PPME_SYSCALL_PIPE_E:
		case PPME_SYSCALL_PIPE2_E:
			m_tcstr[0] = CHAR_FD_FIFO;
			m_tcstr[1] = 0;
			return m_tcstr;
		case PPME_SYSCALL_EVENTFD_E:
		case PPME_SYSCALL_EVENTFD2_E:
			m_tcstr[0] = CHAR_FD_EVENT;
			m_tcstr[1] = 0;
			return m_tcstr;
		case PPME_SYSCALL_SIGNALFD_E:
		case PPME_SYSCALL_SIGNALFD4_E:
			m_tcstr[0] = CHAR_FD_SIGNAL;
			m_tcstr[1] = 0;
			return m_tcstr;
		case PPME_SYSCALL_TIMERFD_CREATE_E:
			m_tcstr[0] = CHAR_FD_TIMERFD;
			m_tcstr[1] = 0;
			return m_tcstr;
		case PPME_SYSCALL_INOTIFY_INIT_E:
		case PPME_SYSCALL_INOTIFY_INIT1_E:
			m_tcstr[0] = CHAR_FD_INOTIFY;
			m_tcstr[1] = 0;
			return m_tcstr;
		default:
			m_tcstr[0] = 'o';
			m_tcstr[1] = 0;
			return m_tcstr;
		}
	case TYPE_FDNAMERAW:
		{
			if(extract_fdname_from_creator(evt, len, sanitize_strings, true) == true)
			{
				remove_duplicate_path_separators(m_tstr);
				RETURN_EXTRACT_STRING(m_tstr);
			}
			else
			{
				return NULL;
			}
		}
	default:
		return NULL;
	}
}

bool sinsp_filter_check_fd::extract(sinsp_evt *evt, OUT std::vector<extract_value_t>& values, bool sanitize_strings)
{
	values.clear();

	if(!extract_fd(evt))
	{
		return false;
	}

	if(m_field_id == TYPE_FDTYPES && m_argid == -1)
	{
		// We are of the form fd.types so gather all open file
		// descriptor types into a (de-duplicated) list
		//
		// Note that fd.types[num] handling is in the following
		// implementation of sinsp_filter_check_fd::extract

		// All of the pointers come from the fd_typesting() function so
		// we shouldn't have the situation of two distinct pointers to
		// the same string literal and we can just compare based on pointer
		std::unordered_set<char*> fd_types;

		// Iterate over the list of open file descriptors and add all
		// unique file descriptor types to the vector for comparison
		auto fd_type_gather = [this, &fd_types, &values](uint64_t, const sinsp_fdinfo_t& fdinfo)
		{
			char* type = fdinfo.get_typestring();

			if (fd_types.emplace(type).second)
			{
				extract_value_t val;
				val.ptr = (uint8_t*)type;
				val.len = strlen(type);

				values.push_back(val);
			}

			return true;
		};

		m_tinfo->loop_fds(fd_type_gather);

		return true;
	}

	return sinsp_filter_check::extract(evt, values, sanitize_strings);
}

uint8_t* sinsp_filter_check_fd::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	ASSERT(evt);

	if(!extract_fd(evt))
	{
		return NULL;
	}

	//
	// TYPE_FDNUM doesn't need fdinfo
	//
	if(m_field_id == TYPE_FDNUM)
	{
		RETURN_EXTRACT_VAR(m_tinfo->m_lastevent_fd);
	}

	switch(m_field_id)
	{
	case TYPE_FDNAME:
	case TYPE_CONTAINERNAME:
		if(m_fdinfo == NULL)
		{
			return extract_from_null_fd(evt, len, sanitize_strings);
		}

		if(evt->get_type() == PPME_SOCKET_CONNECT_X)
		{
			sinsp_evt_param *parinfo;

			parinfo = evt->get_param(0);
			ASSERT(parinfo->m_len == sizeof(uint64_t));
			int64_t retval = *(int64_t*)parinfo->m_val;

			if(retval < 0)
			{
				return extract_from_null_fd(evt, len, sanitize_strings);
			}
		}

		if(m_field_id == TYPE_CONTAINERNAME)
		{
			ASSERT(m_tinfo != NULL);
			m_tstr = m_tinfo->m_container_id + ':' + m_fdinfo->m_name;
		}
		else
		{
			m_tstr = m_fdinfo->m_name;
		}

		if(sanitize_strings)
		{
			sanitize_string(m_tstr);
		}
		RETURN_EXTRACT_STRING(m_tstr);
		break;
	case TYPE_FDTYPES:
	case TYPE_FDTYPE:
		if(m_fdinfo == NULL)
		{
			return NULL;
		}
		else
		{
			uint8_t *typestr = (uint8_t*)m_fdinfo->get_typestring();
			RETURN_EXTRACT_CSTR(typestr);
		}
		break;
	case TYPE_DIRECTORY:
	case TYPE_CONTAINERDIRECTORY:
		{
			if(m_fdinfo == NULL)
			{
				return extract_from_null_fd(evt, len, sanitize_strings);
			}

			if(!(m_fdinfo->is_file() || m_fdinfo->is_directory()))
			{
				return NULL;
			}

			m_tstr = m_fdinfo->m_name;
			if(sanitize_strings)
			{
				sanitize_string(m_tstr);
			}

			if(m_fdinfo->is_file())
			{
				size_t pos = m_tstr.rfind('/');
				if(pos != string::npos && pos != 0)
				{
					if(pos < m_tstr.size() - 1)
					{
						m_tstr.resize(pos);
					}
				}
				else
				{
					m_tstr = "/";
				}
			}

			if(m_field_id == TYPE_CONTAINERDIRECTORY)
			{
				m_tstr = m_tinfo->m_container_id + ':' + m_tstr;
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	case TYPE_FILENAME:
		{
			if(m_fdinfo == NULL)
			{
				return extract_from_null_fd(evt, len, sanitize_strings);
			}

			if(!m_fdinfo->is_file())
			{
				return NULL;
			}

			m_tstr = m_fdinfo->m_name;
			if(sanitize_strings)
			{
				sanitize_string(m_tstr);
			}

			size_t pos = m_tstr.rfind('/');
			if(pos != string::npos)
			{
				if(pos < m_tstr.size() - 1)
				{
					m_tstr = m_tstr.substr(pos + 1, string::npos);
				}
			}
			else
			{
				m_tstr = "/";
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	case TYPE_FDTYPECHAR:
		if(m_fdinfo == NULL)
		{
			return extract_from_null_fd(evt, len, sanitize_strings);
		}

		*len = 1;
		m_tcstr[0] = m_fdinfo->get_typechar();
		m_tcstr[1] = 0;
		return m_tcstr;
	case TYPE_CNET:
	case TYPE_CLIENTIP:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;
			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip);
			}
			else if (evt_type == SCAP_FD_IPV6_SOCK)
			{
				RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip);
			}
		}
		break;
	case TYPE_CLIENTIP_NAME:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			m_tstr.clear();
			scap_fd_type evt_type = m_fdinfo->m_type;
			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				m_tstr = sinsp_dns_manager::get().name_of(AF_INET, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip, evt->get_ts());
			}
			else if (evt_type == SCAP_FD_IPV6_SOCK)
			{
				m_tstr = sinsp_dns_manager::get().name_of(AF_INET6, &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip.m_b[0], evt->get_ts());
			}

			if(!m_tstr.empty())
			{
				RETURN_EXTRACT_STRING(m_tstr);
			}
		}
		break;
	case TYPE_SNET:
	case TYPE_SERVERIP:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;
			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip);
			}
			else if(evt_type == SCAP_FD_IPV4_SERVSOCK)
			{
				RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4serverinfo.m_ip);
			}
			else if(evt_type == SCAP_FD_IPV6_SOCK)
			{
				RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip);
			}
			else if(evt_type == SCAP_FD_IPV6_SERVSOCK)
			{
				RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6serverinfo.m_ip);
			}
		}
		break;
	case TYPE_SERVERIP_NAME:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			m_tstr.clear();
			scap_fd_type evt_type = m_fdinfo->m_type;
			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				m_tstr = sinsp_dns_manager::get().name_of(AF_INET, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip, evt->get_ts());
			}
			else if(evt_type == SCAP_FD_IPV4_SERVSOCK)
			{
				m_tstr = sinsp_dns_manager::get().name_of(AF_INET, &m_fdinfo->m_sockinfo.m_ipv4serverinfo.m_ip, evt->get_ts());
			}
			else if (evt_type == SCAP_FD_IPV6_SOCK)
			{
				m_tstr = sinsp_dns_manager::get().name_of(AF_INET6, &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip.m_b[0], evt->get_ts());
			}
			else if(evt_type == SCAP_FD_IPV6_SERVSOCK)
			{
				m_tstr = sinsp_dns_manager::get().name_of(AF_INET6, &m_fdinfo->m_sockinfo.m_ipv6serverinfo.m_ip.m_b[0], evt->get_ts());
			}

			if(!m_tstr.empty())
			{
				RETURN_EXTRACT_STRING(m_tstr);
			}
		}
		break;
	case TYPE_LNET:
	case TYPE_RNET:
	case TYPE_LIP:
	case TYPE_RIP:
	case TYPE_LIP_NAME:
	case TYPE_RIP_NAME:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;
			if(evt_type != SCAP_FD_IPV4_SOCK &&
			   evt_type != SCAP_FD_IPV6_SOCK)
			{
				return NULL;
			}

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			/* With local we mean that the client address corresponds to one of our local interfaces */
			bool is_local;

			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				is_local = m_inspector->get_ifaddr_list().is_ipv4addr_in_local_machine(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip, m_tinfo);
			}
			else
			{
				is_local = m_inspector->get_ifaddr_list().is_ipv6addr_in_local_machine(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip, m_tinfo);
			}

			if(m_field_id != TYPE_LIP_NAME && m_field_id != TYPE_RIP_NAME)
			{
				if(is_local)
				{
					if(m_field_id == TYPE_LIP || m_field_id == TYPE_LNET)
					{
						if(evt_type == SCAP_FD_IPV4_SOCK)
						{
							RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip);
						}
						else
						{
							RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip);
						}
					}
					else
					{
						if(evt_type == SCAP_FD_IPV4_SOCK)
						{
							RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip);
						}
						else
						{
							RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip);
						}
					}
				}
				else
				{
					if(m_field_id == TYPE_LIP || m_field_id == TYPE_LNET)
					{
						if(evt_type == SCAP_FD_IPV4_SOCK)
						{
							RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip);
						}
						else
						{
							RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip);
						}
					}
					else
					{
						if(evt_type == SCAP_FD_IPV4_SOCK)
						{
							RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip);
						}
						else
						{
							RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip);
						}
					}
				}
			}
			else
			{
				m_tstr.clear();
				if(is_local)
				{
					if(m_field_id == TYPE_LIP_NAME)
					{
						if(evt_type == SCAP_FD_IPV4_SOCK)
						{
							m_tstr = sinsp_dns_manager::get().name_of(AF_INET, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip, evt->get_ts());
						}
						else
						{
							m_tstr = sinsp_dns_manager::get().name_of(AF_INET6, &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip.m_b[0], evt->get_ts());
						}
					}
					else
					{
						if(evt_type == SCAP_FD_IPV4_SOCK)
						{
							m_tstr = sinsp_dns_manager::get().name_of(AF_INET, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip, evt->get_ts());
						}
						else
						{
							m_tstr = sinsp_dns_manager::get().name_of(AF_INET6, &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip.m_b[0], evt->get_ts());
						}
					}
				}
				else
				{
					if(m_field_id == TYPE_LIP_NAME)
					{
						if(evt_type == SCAP_FD_IPV4_SOCK)
						{
							m_tstr = sinsp_dns_manager::get().name_of(AF_INET, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip, evt->get_ts());
						}
						else
						{
							m_tstr = sinsp_dns_manager::get().name_of(AF_INET6, &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip.m_b[0], evt->get_ts());
						}
					}
					else
					{
						if(evt_type == SCAP_FD_IPV4_SOCK)
						{
							m_tstr = sinsp_dns_manager::get().name_of(AF_INET, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip, evt->get_ts());
						}
						else
						{
							m_tstr = sinsp_dns_manager::get().name_of(AF_INET6, &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip.m_b[0], evt->get_ts());
						}
					}
				}

				if(!m_tstr.empty())
				{
					RETURN_EXTRACT_STRING(m_tstr);
				}
			}
		}

		break;
	case TYPE_CLIENTPORT:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport);
			}
			else if(evt_type == SCAP_FD_IPV6_SOCK)
			{
				RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sport);
			}
		}
		break;
	case TYPE_CLIENTPROTO:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			m_tstr = "";
			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				m_tstr = port_to_string(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport, this->m_fdinfo->get_l4proto(), m_inspector->m_hostname_and_port_resolution_enabled);
			}
			else if(evt_type == SCAP_FD_IPV6_SOCK)
			{
				m_tstr = port_to_string(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sport, this->m_fdinfo->get_l4proto(), m_inspector->m_hostname_and_port_resolution_enabled);
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	case TYPE_SERVERPORT:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;

			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				if(m_fdinfo->is_role_none())
				{
					return NULL;
				}

				RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport);
			}
			else if(evt_type == SCAP_FD_IPV4_SERVSOCK)
			{
				RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4serverinfo.m_port);
			}
			else if(evt_type == SCAP_FD_IPV6_SOCK)
			{
				if(m_fdinfo->is_role_none())
				{
					return NULL;
				}

				RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dport);
			}
			else if(evt_type == SCAP_FD_IPV6_SERVSOCK)
			{
				RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6serverinfo.m_port);
			}
			else
			{
				return NULL;
			}
		}
		break;
	case TYPE_SERVERPROTO:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			uint16_t nport = 0;

			scap_fd_type evt_type = m_fdinfo->m_type;

			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				if(m_fdinfo->is_role_none())
				{
					return NULL;
				}
				nport = m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport;
			}
			else if(evt_type == SCAP_FD_IPV4_SERVSOCK)
			{
				nport = m_fdinfo->m_sockinfo.m_ipv4serverinfo.m_port;
			}
			else if(evt_type == SCAP_FD_IPV6_SOCK)
			{
				if(m_fdinfo->is_role_none())
				{
					return NULL;
				}
				nport = m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dport;
			}
			else if(evt_type == SCAP_FD_IPV6_SERVSOCK)
			{
				nport = m_fdinfo->m_sockinfo.m_ipv6serverinfo.m_port;
			}
			else
			{
				return NULL;
			}

			m_tstr = "";
			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				m_tstr = port_to_string(nport, this->m_fdinfo->get_l4proto(), m_inspector->m_hostname_and_port_resolution_enabled);
			}
			else if(evt_type == SCAP_FD_IPV6_SOCK)
			{
				m_tstr = port_to_string(nport, this->m_fdinfo->get_l4proto(), m_inspector->m_hostname_and_port_resolution_enabled);
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	case TYPE_LPORT:
	case TYPE_RPORT:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;
			if(evt_type != SCAP_FD_IPV4_SOCK &&
			   evt_type != SCAP_FD_IPV6_SOCK)
			{
				return NULL;
			}

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			bool is_local;

			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				is_local = m_inspector->get_ifaddr_list().is_ipv4addr_in_local_machine(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip, m_tinfo);
			}
		        else
			{
				is_local = m_inspector->get_ifaddr_list().is_ipv6addr_in_local_machine(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip, m_tinfo);
			}

 	        if(is_local)
			{
				if(m_field_id == TYPE_LPORT || m_field_id == TYPE_LPROTO)
				{
					if(evt_type == SCAP_FD_IPV4_SOCK)
					{
						RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport);
					}
					else
					{
						RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sport);
					}
				}
				else
				{
					if(evt_type == SCAP_FD_IPV4_SOCK)
					{
						RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport);
					}
					else
					{
						RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dport);
					}
				}
			}
			else
			{
				if(m_field_id == TYPE_LPORT || m_field_id == TYPE_LPROTO)
				{
					if(evt_type == SCAP_FD_IPV4_SOCK)
					{
						RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport);
					}
					else
					{
						RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dport);
					}
				}
				else
				{
					if(evt_type == SCAP_FD_IPV4_SOCK)
					{
						RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport);
					}
					else
					{
						RETURN_EXTRACT_VAR(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sport);
					}
				}
			}
		}
		break;

	case TYPE_LPROTO:
	case TYPE_RPROTO:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			scap_fd_type evt_type = m_fdinfo->m_type;
			if(evt_type != SCAP_FD_IPV4_SOCK &&
			   evt_type != SCAP_FD_IPV6_SOCK)
			{
				return NULL;
			}

			if(m_fdinfo->is_role_none())
			{
				return NULL;
			}

			int16_t nport = 0;

			bool is_local;

			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				is_local = m_inspector->get_ifaddr_list().is_ipv4addr_in_local_machine(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip, m_tinfo);
			}
		        else
			{
				is_local = m_inspector->get_ifaddr_list().is_ipv6addr_in_local_machine(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip, m_tinfo);
			}

                        if(is_local)
			{
				if(m_field_id == TYPE_LPORT || m_field_id == TYPE_LPROTO)
				{
					if(evt_type == SCAP_FD_IPV4_SOCK)
					{
						nport = m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport;
					}
					else
					{
						nport = m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sport;
					}
				}
				else
				{
					if(evt_type == SCAP_FD_IPV4_SOCK)
					{
						nport = m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport;
					}
					else
					{
						nport = m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dport;
					}
				}
			}
			else
			{
				if(m_field_id == TYPE_LPORT || m_field_id == TYPE_LPROTO)
				{
					if(evt_type == SCAP_FD_IPV4_SOCK)
					{
						nport = m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport;
					}
					else
					{
						nport = m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dport;
					}
				}
				else
				{
					if(evt_type == SCAP_FD_IPV4_SOCK)
					{
						nport = m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport;
					}
					else
					{
						nport = m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sport;
					}

				}
			}

			m_tstr = port_to_string(nport, this->m_fdinfo->get_l4proto(), m_inspector->m_hostname_and_port_resolution_enabled);
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;

	case TYPE_L4PROTO:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			scap_l4_proto l4p = m_fdinfo->get_l4proto();

			switch(l4p)
			{
			case SCAP_L4_TCP:
				m_tstr = "tcp";
				break;
			case SCAP_L4_UDP:
				m_tstr = "udp";
				break;
			case SCAP_L4_ICMP:
				m_tstr = "icmp";
				break;
			case SCAP_L4_RAW:
				m_tstr = "raw";
				break;
			default:
				m_tstr = "<NA>";
				break;
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	case TYPE_IS_SERVER:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			if(m_fdinfo->m_type == SCAP_FD_IPV4_SERVSOCK || m_fdinfo->m_type == SCAP_FD_IPV6_SERVSOCK)
			{
				m_tbool = true;
			}
			else if(m_fdinfo->m_type == SCAP_FD_IPV4_SOCK)
			{
				m_tbool =
					m_inspector->get_ifaddr_list().is_ipv4addr_in_local_machine(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip, m_tinfo);
			}
			else if(m_fdinfo->m_type == SCAP_FD_IPV6_SOCK)
			{
				m_tbool =
					m_inspector->get_ifaddr_list().is_ipv6addr_in_local_machine(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip, m_tinfo);
			}
			else
			{
				m_tbool = false;
			}

			RETURN_EXTRACT_VAR(m_tbool);
		}
		break;
	case TYPE_SOCKFAMILY:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			if(m_fdinfo->m_type == SCAP_FD_IPV4_SOCK || m_fdinfo->m_type == SCAP_FD_IPV6_SOCK ||
			   m_fdinfo->m_type == SCAP_FD_IPV4_SERVSOCK || m_fdinfo->m_type == SCAP_FD_IPV6_SERVSOCK)
			{
				m_tstr = "ip";
				RETURN_EXTRACT_STRING(m_tstr);
			}
			else if(m_fdinfo->m_type == SCAP_FD_UNIX_SOCK)
			{
				m_tstr = "unix";
				RETURN_EXTRACT_STRING(m_tstr);
			}
			else
			{
				return NULL;
			}
		}
		break;
	case TYPE_UID:
		{
			if(m_tinfo == nullptr)
			{
				return NULL;
			}

			m_tstr = to_string(m_tinfo->m_tid) + to_string(m_tinfo->m_lastevent_fd);
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	case TYPE_IS_CONNECTED:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			m_tbool = m_fdinfo->is_socket_connected();

			RETURN_EXTRACT_VAR(m_tbool);
		}
		break;
	case TYPE_NAME_CHANGED:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			m_tbool = evt->fdinfo_name_changed();

			RETURN_EXTRACT_VAR(m_tbool);
		}
		break;
	case TYPE_DEV:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			m_tbool = m_fdinfo->get_device();

			RETURN_EXTRACT_VAR(m_tbool);
		}
		break;
	case TYPE_DEV_MAJOR:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			m_tbool = m_fdinfo->get_device_major();

			RETURN_EXTRACT_VAR(m_tbool);
		}
		break;
	case TYPE_DEV_MINOR:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			m_tbool = m_fdinfo->get_device_minor();

			RETURN_EXTRACT_VAR(m_tbool);
		}
		break;
	case TYPE_INO:
		{
			if(m_fdinfo == NULL)
			{
				return NULL;
			}

			m_conv_uint64 = m_fdinfo->get_ino();

			RETURN_EXTRACT_VAR(m_conv_uint64);
		}
		break;
	case TYPE_FDNAMERAW:
		{
		if(m_fdinfo == NULL)
		{
			return extract_from_null_fd(evt, len, sanitize_strings);
		}

		m_tstr = m_fdinfo->m_name_raw;
		remove_duplicate_path_separators(m_tstr);
		RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	default:
		ASSERT(false);
	}

	return NULL;
}

bool sinsp_filter_check_fd::compare_ip(sinsp_evt *evt)
{
	if(!extract_fd(evt))
	{
		return false;
	}

	if(m_fdinfo != NULL)
	{
		scap_fd_type evt_type = m_fdinfo->m_type;

		if(evt_type == SCAP_FD_IPV4_SOCK)
		{
			if(m_cmpop == CO_EQ || m_cmpop == CO_IN)
			{
				if(flt_compare(m_cmpop, PT_IPV4ADDR, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip) ||
					flt_compare(m_cmpop, PT_IPV4ADDR, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip))
				{
					return true;
				}
			}
			else if(m_cmpop == CO_NE)
			{
				if(flt_compare(m_cmpop, PT_IPV4ADDR, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip) &&
					flt_compare(m_cmpop, PT_IPV4ADDR, &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip))
				{
					return true;
				}
			}
			else
			{
				throw sinsp_exception("filter error: IP filter only supports '=' and '!=' operators");
			}
		}
		else if(evt_type == SCAP_FD_IPV4_SERVSOCK)
		{
			if(m_cmpop == CO_EQ || m_cmpop == CO_NE || m_cmpop == CO_IN)
			{
				return flt_compare(m_cmpop, PT_IPV4ADDR, &m_fdinfo->m_sockinfo.m_ipv4serverinfo.m_ip);
			}
			else
			{
				throw sinsp_exception("filter error: IP filter only supports '=' and '!=' operators");
			}
		}
		else if(evt_type == SCAP_FD_IPV6_SOCK)
		{
			if(m_cmpop == CO_EQ || m_cmpop == CO_IN)
			{
				if(flt_compare(m_cmpop, PT_IPV6ADDR, &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip) ||
					flt_compare(m_cmpop, PT_IPV6ADDR, &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip))
				{
					return true;
				}
			}
			else if(m_cmpop == CO_NE)
			{
				if(flt_compare(m_cmpop, PT_IPV6ADDR, &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip) &&
					flt_compare(m_cmpop, PT_IPV6ADDR, &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip))
				{
					return true;
				}
			}
			else
			{
				throw sinsp_exception("filter error: IP filter only supports '=' and '!=' operators");
			}
		}
		else if(evt_type == SCAP_FD_IPV6_SERVSOCK)
		{
			if(m_cmpop == CO_EQ || m_cmpop == CO_NE || m_cmpop == CO_IN)
			{
				return flt_compare(m_cmpop, PT_IPV6ADDR, &m_fdinfo->m_sockinfo.m_ipv6serverinfo.m_ip);
			}
			else
			{
				throw sinsp_exception("filter error: IP filter only supports '=' and '!=' operators");
			}
		}
	}

	return false;
}

bool sinsp_filter_check_fd::compare_net(sinsp_evt *evt)
{
	if(!extract_fd(evt) || m_fdinfo == nullptr)
	{
		return false;
	}

	bool sip_cmp = false;
	bool dip_cmp = false;

	switch (m_fdinfo->m_type)
	{
	case SCAP_FD_IPV4_SERVSOCK:
		return flt_compare_ipv4net(m_cmpop, m_fdinfo->m_sockinfo.m_ipv4serverinfo.m_ip, (ipv4net*)filter_value_p());

	case SCAP_FD_IPV6_SERVSOCK:
		return flt_compare_ipv6net(m_cmpop, &m_fdinfo->m_sockinfo.m_ipv6serverinfo.m_ip, (ipv6net*)filter_value_p());

	case SCAP_FD_IPV4_SOCK:
		sip_cmp = flt_compare_ipv4net(m_cmpop, m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip, (ipv4net*)filter_value_p());
		dip_cmp = flt_compare_ipv4net(m_cmpop, m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip, (ipv4net*)filter_value_p());
		break;

	case SCAP_FD_IPV6_SOCK:
		sip_cmp = flt_compare_ipv6net(m_cmpop, &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip, (ipv6net*)filter_value_p());
		dip_cmp = flt_compare_ipv6net(m_cmpop, &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip, (ipv6net*)filter_value_p());
		break;

	default:
		return false;
	}

	if(m_cmpop == CO_EQ || m_cmpop == CO_IN)
	{
		return sip_cmp || dip_cmp;
	}

	if(m_cmpop == CO_NE)
	{
		return sip_cmp && dip_cmp;
	}

	return false;
}

bool sinsp_filter_check_fd::compare_port(sinsp_evt *evt)
{
	if(!extract_fd(evt))
	{
		return false;
	}

	if(m_fdinfo != NULL)
	{
		uint16_t* sport;
		uint16_t* dport;
		scap_fd_type evt_type = m_fdinfo->m_type;

		if(evt_type == SCAP_FD_IPV4_SOCK)
		{
			sport = &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport;
			dport = &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport;
		}
		else if(evt_type == SCAP_FD_IPV4_SERVSOCK)
		{
			sport = &m_fdinfo->m_sockinfo.m_ipv4serverinfo.m_port;
			dport = &m_fdinfo->m_sockinfo.m_ipv4serverinfo.m_port;
		}
		else if(evt_type == SCAP_FD_IPV6_SOCK)
		{
			sport = &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sport;
			dport = &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dport;
		}
		else if(evt_type == SCAP_FD_IPV6_SERVSOCK)
		{
			sport = &m_fdinfo->m_sockinfo.m_ipv6serverinfo.m_port;
			dport = &m_fdinfo->m_sockinfo.m_ipv6serverinfo.m_port;
		}
		else
		{
			return false;
		}

		switch(m_cmpop)
		{
		case CO_EQ:
			if(*sport == *(uint16_t*)filter_value_p() ||
				*dport == *(uint16_t*)filter_value_p())
			{
				return true;
			}
			break;
		case CO_NE:
			if(*sport != *(uint16_t*)filter_value_p() &&
				*dport != *(uint16_t*)filter_value_p())
			{
				return true;
			}
			break;
		case CO_LT:
			if(*sport < *(uint16_t*)filter_value_p() ||
				*dport < *(uint16_t*)filter_value_p())
			{
				return true;
			}
			break;
		case CO_LE:
			if(*sport <= *(uint16_t*)filter_value_p() ||
				*dport <= *(uint16_t*)filter_value_p())
			{
				return true;
			}
			break;
		case CO_GT:
			if(*sport > *(uint16_t*)filter_value_p() ||
				*dport > *(uint16_t*)filter_value_p())
			{
				return true;
			}
			break;
		case CO_GE:
			if(*sport >= *(uint16_t*)filter_value_p() ||
				*dport >= *(uint16_t*)filter_value_p())
			{
				return true;
			}
			break;

		case CO_IN:
			if(flt_compare(m_cmpop,
				       PT_PORT,
				       sport,
				       sizeof(*sport)) ||
			   flt_compare(m_cmpop,
				       PT_PORT,
				       dport,
				       sizeof(*dport)))
			{
				return true;
			}
			break;
		default:
			throw sinsp_exception("filter error: unsupported port comparison operator");
		}
	}

	return false;
}

bool sinsp_filter_check_fd::compare_domain(sinsp_evt *evt)
{
	if(!extract_fd(evt))
	{
		return false;
	}

	if(m_fdinfo != NULL)
	{
		scap_fd_type evt_type = m_fdinfo->m_type;
		if(evt_type != SCAP_FD_IPV4_SOCK &&
		   evt_type != SCAP_FD_IPV6_SOCK)
		{
			return false;
		}

		if(m_fdinfo->is_role_none())
		{
			return false;
		}

		uint32_t *addr;
		if(m_field_id == TYPE_CLIENTIP_NAME)
		{
			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				addr = &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip;
			}
			else
			{
				addr = &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip.m_b[0];
			}
		}
		else if(m_field_id == TYPE_SERVERIP_NAME)
		{
			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				addr = &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip;
			}
			else
			{
				addr = &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip.m_b[0];
			}
		}
		else
		{
			bool is_local;
			if(evt_type == SCAP_FD_IPV4_SOCK)
			{
				is_local = m_inspector->get_ifaddr_list().is_ipv4addr_in_local_machine(m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip, m_tinfo);
			}
			else
			{
				is_local = m_inspector->get_ifaddr_list().is_ipv6addr_in_local_machine(m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip, m_tinfo);
			}

			if(is_local)
			{
				if(m_field_id == TYPE_LIP_NAME)
				{
					if(evt_type == SCAP_FD_IPV4_SOCK)
					{
						addr = &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip;
					}
					else
					{
						addr = &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip.m_b[0];
					}
				}
				else
				{
					if(evt_type == SCAP_FD_IPV4_SOCK)
					{
						addr = &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip;
					}
					else
					{
						addr = &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip.m_b[0];
					}
				}
			}
			else
			{
				if(m_field_id == TYPE_LIP_NAME)
				{
					if(evt_type == SCAP_FD_IPV4_SOCK)
					{
						addr = &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip;
					}
					else
					{
						addr = &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip.m_b[0];
					}
				}
				else
				{
					if(evt_type == SCAP_FD_IPV4_SOCK)
					{
						addr = &m_fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip;
					}
					else
					{
						addr = &m_fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip.m_b[0];
					}
				}
			}
		}

		uint64_t ts = evt->get_ts();

		if(m_cmpop == CO_IN)
		{
			for (uint16_t i=0; i < m_val_storages.size(); i++)
			{
				if(sinsp_dns_manager::get().match((const char *)filter_value_p(i), (evt_type == SCAP_FD_IPV6_SOCK)? AF_INET6 : AF_INET, addr, ts))
				{
					return true;
				}
			}

			return false;
		}
		else if(m_cmpop == CO_EQ)
		{
			return sinsp_dns_manager::get().match((const char *)filter_value_p(), (evt_type == SCAP_FD_IPV6_SOCK)? AF_INET6 : AF_INET, addr, ts);
		}
		else if(m_cmpop == CO_NE)
		{
			return !sinsp_dns_manager::get().match((const char *)filter_value_p(), (evt_type == SCAP_FD_IPV6_SOCK)? AF_INET6 : AF_INET, addr, ts);
		}
		else
		{
			throw sinsp_exception("filter error: fd.*ip.name filter only supports '=' and '!=' operators");
		}
	}

	return false;
}

bool sinsp_filter_check_fd::extract_fd(sinsp_evt *evt)
{
	ppm_event_flags eflags = evt->get_info_flags();

	//
	// Make sure this is an event that creates or consumes an fd
	//
	if(eflags & (EF_CREATES_FD | EF_USES_FD | EF_DESTROYS_FD))
	{
		//
		// This is an fd-related event, get the thread info and the fd info
		//
		m_tinfo = evt->get_thread_info();
		if(m_tinfo == NULL)
		{
			return false;
		}

		if (m_argid != -1)
		{
			m_fdinfo = m_tinfo->get_fd(m_argid);
		}
		else
		{
			m_fdinfo = evt->get_fd_info();

			if (m_fdinfo == NULL && m_tinfo->m_lastevent_fd != -1)
			{
				m_fdinfo = m_tinfo->get_fd(m_tinfo->m_lastevent_fd);
			}
		}
		// We'll check if fd is null below
	}
	else
	{
		return false;
	}

	return true;
}

bool sinsp_filter_check_fd::compare(sinsp_evt *evt)
{
	//
	// Some fields are filter only and therefore get a special treatment
	//
	if(m_field_id == TYPE_IP)
	{
		return compare_ip(evt);
	}
	else if(m_field_id == TYPE_PORT || m_field_id == TYPE_PROTO)
	{
		return compare_port(evt);
	}
	else if(m_field_id == TYPE_NET)
	{
		return compare_net(evt);
	}
	else if(m_field_id == TYPE_FDTYPES)
	{
		m_extracted_values.clear();
		if(!extract_cached(evt, m_extracted_values, false))
		{
			return false;
		}
		return flt_compare(m_cmpop, m_info.m_fields[m_field_id].m_type, m_extracted_values);
	}

	//
	// Standard extract-based fields
	//
	uint32_t len = 0;
	bool sanitize_strings = false;
	// note: this uses the single-value extract because this filtercheck
	// class does not support multi-valued extraction
	uint8_t* extracted_val = extract(evt, &len, sanitize_strings);

	if(extracted_val == NULL)
	{
		// optimization for *_NAME fields
		// the first time we will call compare_domain, the next ones
		// we will the able to extract and use flt_compare
		if(m_field_id == TYPE_CLIENTIP_NAME ||
		   m_field_id == TYPE_SERVERIP_NAME ||
		   m_field_id == TYPE_LIP_NAME ||
		   m_field_id == TYPE_RIP_NAME)
		{
			return compare_domain(evt);
		}

		return false;
	}

	return flt_compare(m_cmpop,
			   m_info.m_fields[m_field_id].m_type,
			   extracted_val,
			   len);
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_thread implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_thread_fields[] =
{
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.exe", "First Argument", "The first command line argument argv[0] (truncated after 4096 bytes) which is usually the executable name but it could be also a custom string, it depends on what the user specifies. This field is collected from the syscalls args or, as a fallback, extracted from /proc/<pid>/cmdline."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.pexe", "Parent First Argument", "The proc.exe (first command line argument argv[0]) of the parent process."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.aexe", "Ancestor First Argument", "The proc.exe (first command line argument argv[0]) for a specific process ancestor. You can access different levels of ancestors by using indices. For example, proc.aexe[1] retrieves the proc.exe of the parent process, proc.aexe[2] retrieves the proc.exe of the grandparent process, and so on. The current process's proc.exe line can be obtained using proc.aexe[0]. When used without any arguments, proc.aexe is applicable only in filters and matches any of the process ancestors. For instance, you can use `proc.aexe endswith java` to match any process ancestor whose proc.exe ends with the term `java`."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.exepath", "Process Executable Path", "The full executable path of the process (it could be truncated after 1024 bytes if read from '/proc'). This field is collected directly from the kernel or, as a fallback, extracted resolving the path of /proc/<pid>/exe, so symlinks are resolved. If you are using eBPF drivers this path could be truncated due to verifier complexity limits. (legacy eBPF kernel version < 5.2) truncated after 24 path components. (legacy eBPF kernel version >= 5.2) truncated after 48 path components. (modern eBPF kernel) truncated after 96 path components."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.pexepath", "Parent Process Executable Path", "The proc.exepath (full executable path) of the parent process."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.aexepath", "Ancestor Executable Path", "The proc.exepath (full executable path) for a specific process ancestor. You can access different levels of ancestors by using indices. For example, proc.aexepath[1] retrieves the proc.exepath of the parent process, proc.aexepath[2] retrieves the proc.exepath of the grandparent process, and so on. The current process's proc.exepath line can be obtained using proc.aexepath[0]. When used without any arguments, proc.aexepath is applicable only in filters and matches any of the process ancestors. For instance, you can use `proc.aexepath endswith java` to match any process ancestor whose path ends with the term `java`."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.name", "Name", "The process name (truncated after 16 characters) generating the event (task->comm). This field is collected from the syscalls args or, as a fallback, extracted from /proc/<pid>/status. The name of the process and the name of the executable file on disk (if applicable) can be different if a process is given a custom name which is often the case for example for java applications."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.pname", "Parent Name", "The proc.name truncated after 16 characters) of the process generating the event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.aname", "Ancestor Name", "The proc.name (truncated after 16 characters) for a specific process ancestor. You can access different levels of ancestors by using indices. For example, proc.aname[1] retrieves the proc.name of the parent process, proc.aname[2] retrieves the proc.name of the grandparent process, and so on. The current process's proc.name line can be obtained using proc.aname[0]. When used without any arguments, proc.aname is applicable only in filters and matches any of the process ancestors. For instance, you can use `proc.aname=bash` to match any process ancestor whose name is `bash`."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.args", "Arguments", "The arguments passed on the command line when starting the process generating the event excluding argv[0] (truncated after 4096 bytes). This field is collected from the syscalls args or, as a fallback, extracted from /proc/<pid>/cmdline."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.cmdline", "Command Line", "The concatenation of `proc.name + proc.args` (truncated after 4096 bytes) when starting the process generating the event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.pcmdline", "Parent Command Line", "The proc.cmdline (full command line (proc.name + proc.args)) of the parent of the process generating the event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.acmdline", "Ancestor Command Line", "The full command line (proc.name + proc.args) for a specific process ancestor. You can access different levels of ancestors by using indices. For example, proc.acmdline[1] retrieves the full command line of the parent process, proc.acmdline[2] retrieves the proc.cmdline of the grandparent process, and so on. The current process's full command line can be obtained using proc.acmdline[0].  When used without any arguments, proc.acmdline is applicable only in filters and matches any of the process ancestors. For instance, you can use `proc.acmdline contains base64` to match any process ancestor whose command line contains the term base64."},
	{PT_UINT64, EPF_NONE, PF_DEC, "proc.cmdnargs", "Number of Command Line args", "The number of command line args (proc.args)."},
	{PT_UINT64, EPF_NONE, PF_DEC, "proc.cmdlenargs", "Total Count of Characters in Command Line args", "The total count of characters / length of the comamnd line args (proc.args) combined excluding whitespaces between args."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.exeline", "Executable Command Line", "The full command line, with exe as first argument (proc.exe + proc.args) when starting the process generating the event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.env", "Environment", "The environment variables of the process generating the event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.cwd", "Current Working Directory", "The current working directory of the event."},
	{PT_INT64, EPF_NONE, PF_ID, "proc.loginshellid", "Login Shell ID", "The pid of the oldest shell among the ancestors of the current process, if there is one. This field can be used to separate different user sessions, and is useful in conjunction with chisels like spy_user."},
	{PT_UINT32, EPF_NONE, PF_ID, "proc.tty", "Process TTY", "The controlling terminal of the process. 0 for processes without a terminal."},
	{PT_INT64, EPF_NONE, PF_ID, "proc.pid", "Process ID", "The id of the process generating the event."},
	{PT_INT64, EPF_NONE, PF_ID, "proc.ppid", "Parent Process ID", "The pid of the parent of the process generating the event."},
	{PT_INT64, EPF_NONE, PF_ID, "proc.apid", "Ancestor Process ID", "The pid for a specific process ancestor. You can access different levels of ancestors by using indices. For example, proc.apid[1] retrieves the pid of the parent process, proc.apid[2] retrieves the pid of the grandparent process, and so on. The current process's pid can be obtained using proc.apid[0].  When used without any arguments, proc.acmdline is applicable only in filters and matches any of the process ancestors. For instance, you can use `proc.apid=1337` to match any process ancestor whose pid is equal to 1337."},
	{PT_INT64, EPF_NONE, PF_ID, "proc.vpid", "Virtual Process ID", "The id of the process generating the event as seen from its current PID namespace."},
	{PT_INT64, EPF_NONE, PF_ID, "proc.pvpid", "Parent Virtual Process ID", "The id of the parent process generating the event as seen from its current PID namespace."},
	{PT_INT64, EPF_NONE, PF_ID, "proc.sid", "Process Session ID", "The session id of the process generating the event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.sname", "Process Session Name", "The name of the current process's session leader. This is either the process with pid=proc.sid or the eldest ancestor that has the same sid as the current process."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.sid.exe", "Process Session First Argument", "The first command line argument argv[0] (usually the executable name or a custom one) of the current process's session leader. This is either the process with pid=proc.sid or the eldest ancestor that has the same sid as the current process."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.sid.exepath", "Process Session Executable Path", "The full executable path of the current process's session leader. This is either the process with pid=proc.sid or the eldest ancestor that has the same sid as the current process."},
	{PT_INT64, EPF_NONE, PF_ID, "proc.vpgid", "Process Virtual Group ID", "The process group id of the process generating the event, as seen from its current PID namespace."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.vpgid.name", "Process Group Name", "The name of the current process's process group leader. This is either the process with proc.vpgid == proc.vpid or the eldest ancestor that has the same vpgid as the current process. The description of `proc.is_vpgid_leader` offers additional insights."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.vpgid.exe", "Process Group First Argument", "The first command line argument argv[0] (usually the executable name or a custom one) of the current process's process group leader. This is either the process with proc.vpgid == proc.vpid or the eldest ancestor that has the same vpgid as the current process. The description of `proc.is_vpgid_leader` offers additional insights."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "proc.vpgid.exepath", "Process Group Executable Path", "The full executable path of the current process's process group leader. This is either the process with proc.vpgid == proc.vpid or the eldest ancestor that has the same vpgid as the current process. The description of `proc.is_vpgid_leader` offers additional insights."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "proc.duration", "Process Duration", "Number of nanoseconds since the process started."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "proc.ppid.duration", "Parent Process Duration", "Number of nanoseconds since the parent process started."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "proc.pid.ts", "Process start ts", "Start of process as epoch timestamp in nanoseconds."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "proc.ppid.ts", "Parent Process start ts", "Start of parent process as epoch timestamp in nanoseconds."},
	{PT_BOOL, EPF_NONE, PF_NA, "proc.is_exe_writable", "Process Executable Is Writable", "'true' if this process' executable file is writable by the same user that spawned the process."},
	{PT_BOOL, EPF_NONE, PF_NA, "proc.is_exe_upper_layer", "Process Executable Is In Upper Layer", "'true' if this process' executable file is in upper layer in overlayfs. This field value can only be trusted if the underlying kernel version is greater or equal than 3.18.0, since overlayfs was introduced at that time."},
	{PT_BOOL, EPF_NONE, PF_NA, "proc.is_exe_from_memfd", "Process Executable Is Stored In Memfd", "'true' if the executable file of the current process is an anonymous file created using memfd_create() and is being executed by referencing its file descriptor (fd). This type of file exists only in memory and not on disk. Relevant to detect malicious in-memory code injection. Requires kernel version greater or equal to 3.17.0."},
	{PT_BOOL, EPF_NONE, PF_NA, "proc.is_sid_leader", "Process Is Process Session Leader", "'true' if this process is the leader of the process session, proc.sid == proc.vpid. For host processes vpid reflects pid."},
	{PT_BOOL, EPF_NONE, PF_NA, "proc.is_vpgid_leader", "Process Is Virtual Process Group Leader", "'true' if this process is the leader of the virtual process group, proc.vpgid == proc.vpid. For host processes vpgid and vpid reflect pgid and pid. Can help to distinguish if the process was 'directly' executed for instance in a tty (similar to bash history logging, `is_vpgid_leader` would be 'true') or executed as descendent process in the same process group which for example is the case when subprocesses are spawned from a script (`is_vpgid_leader` would be 'false')."},
	{PT_INT64, EPF_NONE, PF_DEC, "proc.exe_ino", "Inode number of executable file on disk", "The inode number of the executable file on disk. Can be correlated with fd.ino."},
	{PT_ABSTIME, EPF_NONE, PF_DEC, "proc.exe_ino.ctime", "Last status change time (ctime) of executable file", "Last status change time of executable file (inode->ctime) as epoch timestamp in nanoseconds. Time is changed by writing or by setting inode information e.g. owner, group, link count, mode etc."},
	{PT_ABSTIME, EPF_NONE, PF_DEC, "proc.exe_ino.mtime", "Last modification time (mtime) of executable file", "Last modification time of executable file (inode->mtime) as epoch timestamp in nanoseconds. Time is changed by file modifications, e.g. by mknod, truncate, utime, write of more than zero bytes etc. For tracking changes in owner, group, link count or mode, use proc.exe_ino.ctime instead."},
	{PT_ABSTIME, EPF_NONE, PF_DEC, "proc.exe_ino.ctime_duration_proc_start", "Number of nanoseconds between ctime exe file and proc clone ts", "Number of nanoseconds between modifying status of executable image and spawning a new process using the changed executable image."},
	{PT_ABSTIME, EPF_NONE, PF_DEC, "proc.exe_ino.ctime_duration_pidns_start", "Number of nanoseconds between pidns start ts and ctime exe file", "Number of nanoseconds between PID namespace start ts and ctime exe file if PID namespace start predates ctime."},
	{PT_UINT64, EPF_NONE, PF_DEC, "proc.pidns_init_start_ts", "Start ts of pid namespace", "Start of PID namespace (container or non container pid namespace) as epoch timestamp in nanoseconds."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "thread.cap_permitted", "Permitted capabilities", "The permitted capabilities set"},
	{PT_CHARBUF, EPF_NONE, PF_NA, "thread.cap_inheritable", "Inheritable capabilities", "The inheritable capabilities set"},
	{PT_CHARBUF, EPF_NONE, PF_NA, "thread.cap_effective", "Effective capabilities", "The effective capabilities set"},
	{PT_BOOL, EPF_NONE, PF_NA, "proc.is_container_healthcheck", "Process Is Container Healthcheck", "'true' if this process is running as a part of the container's health check."},
	{PT_BOOL, EPF_NONE, PF_NA, "proc.is_container_liveness_probe", "Process Is Container Liveness", "'true' if this process is running as a part of the container's liveness probe."},
	{PT_BOOL, EPF_NONE, PF_NA, "proc.is_container_readiness_probe", "Process Is Container Readiness", "'true' if this process is running as a part of the container's readiness probe."},
	{PT_UINT64, EPF_NONE, PF_DEC, "proc.fdopencount", "FD Count", "Number of open FDs for the process"},
	{PT_INT64, EPF_NONE, PF_DEC, "proc.fdlimit", "FD Limit", "Maximum number of FDs the process can open."},
	{PT_DOUBLE, EPF_NONE, PF_NA, "proc.fdusage", "FD Usage", "The ratio between open FDs and maximum available FDs for the process."},
	{PT_UINT64, EPF_NONE, PF_DEC, "proc.vmsize", "VM Size", "Total virtual memory for the process (as kb)."},
	{PT_UINT64, EPF_NONE, PF_DEC, "proc.vmrss", "VM RSS", "Resident non-swapped memory for the process (as kb)."},
	{PT_UINT64, EPF_NONE, PF_DEC, "proc.vmswap", "VM Swap", "Swapped memory for the process (as kb)."},
	{PT_UINT64, EPF_NONE, PF_DEC, "thread.pfmajor", "Major Page Faults", "Number of major page faults since thread start."},
	{PT_UINT64, EPF_NONE, PF_DEC, "thread.pfminor", "Minor Page Faults", "Number of minor page faults since thread start."},
	{PT_INT64, EPF_NONE, PF_ID, "thread.tid", "Thread ID", "The id of the thread generating the event."},
	{PT_BOOL, EPF_NONE, PF_NA, "thread.ismain", "Main Thread", "'true' if the thread generating the event is the main one in the process."},
	{PT_INT64, EPF_NONE, PF_ID, "thread.vtid", "Virtual Thread ID", "The id of the thread generating the event as seen from its current PID namespace."},
	{PT_CHARBUF, EPF_TABLE_ONLY, PF_NA, "thread.nametid", "Thread Name + ID", "This field chains the process name and tid of a thread and can be used as a specific identifier of a thread for a specific execve."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "thread.exectime", "Scheduled Thread CPU Time", "CPU time spent by the last scheduled thread, in nanoseconds. Exported by switch events only."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "thread.totexectime", "Current Thread CPU Time", "Total CPU time, in nanoseconds since the beginning of the capture, for the current thread. Exported by switch events only."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "thread.cgroups", "Thread Cgroups", "All cgroups the thread belongs to, aggregated into a single string."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "thread.cgroup", "Thread Cgroup", "The cgroup the thread belongs to, for a specific subsystem. e.g. thread.cgroup.cpuacct."},
	{PT_UINT64, EPF_NONE, PF_DEC, "proc.nthreads", "Threads", "The number of alive threads that the process generating the event currently has, including the leader thread. Please note that the leader thread may not be here, in that case 'proc.nthreads' and 'proc.nchilds' are equal"},
	{PT_UINT64, EPF_NONE, PF_DEC, "proc.nchilds", "Children", "The number of alive not leader threads that the process generating the event currently has. This excludes the leader thread."},
	{PT_DOUBLE, EPF_NONE, PF_NA, "thread.cpu", "Thread CPU", "The CPU consumed by the thread in the last second."},
	{PT_DOUBLE, EPF_NONE, PF_NA, "thread.cpu.user", "Thread User CPU", "The user CPU consumed by the thread in the last second."},
	{PT_DOUBLE, EPF_NONE, PF_NA, "thread.cpu.system", "Thread System CPU", "The system CPU consumed by the thread in the last second."},
	{PT_UINT64, EPF_NONE, PF_DEC, "thread.vmsize", "Thread VM Size (kb)", "For the process main thread, this is the total virtual memory for the process (as kb). For the other threads, this field is zero."},
	{PT_UINT64, EPF_NONE, PF_DEC, "thread.vmrss", "Thread VM RSS (kb)", "For the process main thread, this is the resident non-swapped memory for the process (as kb). For the other threads, this field is zero."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "thread.vmsize.b", "Thread VM Size (b)", "For the process main thread, this is the total virtual memory for the process (in bytes). For the other threads, this field is zero."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "thread.vmrss.b", "Thread VM RSS (b)", "For the process main thread, this is the resident non-swapped memory for the process (in bytes). For the other threads, this field is zero."},
};

sinsp_filter_check_thread::sinsp_filter_check_thread()
{
	m_info.m_name = "process";
	m_info.m_desc = "Additional information about the process and thread executing the syscall event.";
	m_info.m_fields = sinsp_filter_check_thread_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_thread_fields) / sizeof(sinsp_filter_check_thread_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;

	m_u64val = 0;
	m_cursec_ts = 0;
}

sinsp_filter_check* sinsp_filter_check_thread::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_thread();
}

int32_t sinsp_filter_check_thread::extract_arg(string fldname, string val, OUT const struct ppm_param_info** parinfo)
{
	uint32_t parsed_len = 0;

	//
	// 'arg' and 'resarg' are handled in a custom way
	//
	if(m_field_id == TYPE_APID ||
		m_field_id == TYPE_ANAME ||
		m_field_id == TYPE_AEXE ||
		m_field_id == TYPE_AEXEPATH ||
		m_field_id == TYPE_ACMDLINE)
	{
		if(val[fldname.size()] == '[')
		{
			parsed_len = (uint32_t)val.find(']');
			string numstr = val.substr(fldname.size() + 1, parsed_len - fldname.size() - 1);
			m_argid = sinsp_numparser::parsed32(numstr);
			parsed_len++;
		}
		else
		{
			throw sinsp_exception("filter syntax error: " + val);
		}
	}
	else if(m_field_id == TYPE_CGROUP)
	{
		if(val[fldname.size()] == '.')
		{
			size_t endpos;
			for(endpos = fldname.size() + 1; endpos < val.length(); ++endpos)
			{
				if(!isalpha(val[endpos])
					&& val[endpos] != '_')
				{
					break;
				}
			}

			parsed_len = (uint32_t)endpos;
			m_argname = val.substr(fldname.size() + 1, endpos - fldname.size() - 1);
		}
		else
		{
			throw sinsp_exception("filter syntax error: " + val);
		}
	}

	return parsed_len;
}

int32_t sinsp_filter_check_thread::parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering)
{
	string val(str);

	if(STR_MATCH("arg"))
	{
		//
		// 'arg' is handled in a custom way
		//
		throw sinsp_exception("filter error: proc.arg filter not implemented yet");
	}
	else if(STR_MATCH("proc.apid"))
	{
		m_field_id = TYPE_APID;
		m_field = &m_info.m_fields[m_field_id];

		int32_t res = 0;

		try
		{
			res = extract_arg("proc.apid", val, NULL);
		}
		catch(...)
		{
			if(val == "proc.apid")
			{
				m_argid = -1;
				res = (int32_t)val.size();
			}
		}

		return res;
	}
	else if(STR_MATCH("proc.aname"))
	{
		m_field_id = TYPE_ANAME;
		m_field = &m_info.m_fields[m_field_id];

		int32_t res = 0;

		try
		{
			res = extract_arg("proc.aname", val, NULL);
		}
		catch(...)
		{
			if(val == "proc.aname")
			{
				m_argid = -1;
				res = (int32_t)val.size();
			}
		}

		return res;
	}
	else if(STR_MATCH("proc.aexepath"))
	{
		m_field_id = TYPE_AEXEPATH;
		m_field = &m_info.m_fields[m_field_id];

		int32_t res = 0;

		try
		{
			res = extract_arg("proc.aexepath", val, NULL);
		}
		catch(...)
		{
			if(val == "proc.aexepath")
			{
				m_argid = -1;
				res = (int32_t)val.size();
			}
		}

		return res;
	}
	/* note: because of str similarity of proc.aexe to proc.aexepath, this needs to be placed after proc.aexepath */
	else if(STR_MATCH("proc.aexe"))
	{
		m_field_id = TYPE_AEXE;
		m_field = &m_info.m_fields[m_field_id];

		int32_t res = 0;

		try
		{
			res = extract_arg("proc.aexe", val, NULL);
		}
		catch(...)
		{
			if(val == "proc.aexe")
			{
				m_argid = -1;
				res = (int32_t)val.size();
			}
		}

		return res;
	}
	else if(STR_MATCH("proc.acmdline"))
	{
		m_field_id = TYPE_ACMDLINE;
		m_field = &m_info.m_fields[m_field_id];

		int32_t res = 0;

		try
		{
			res = extract_arg("proc.acmdline", val, NULL);
		}
		catch(...)
		{
			if(val == "proc.acmdline")
			{
				m_argid = -1;
				res = (int32_t)val.size();
			}
		}

		return res;
	}
	else if(STR_MATCH("thread.totexectime"))
	{
		//
		// Allocate thread storage for the value
		//
		if(alloc_state)
		{
			auto acc = m_inspector->m_thread_manager->dynamic_fields()->add_field<uint64_t>("_tmp_sinsp_filter_thread_totexectime");
			m_thread_dyn_field_accessor.reset(new libsinsp::state::dynamic_struct::field_accessor<uint64_t>(acc.new_accessor<uint64_t>()));
		}

		return sinsp_filter_check::parse_field_name(str, alloc_state, needed_for_filtering);
	}
	else if(STR_MATCH("thread.cgroup") &&
			!STR_MATCH("thread.cgroups"))
	{
		m_field_id = TYPE_CGROUP;
		m_field = &m_info.m_fields[m_field_id];

		return extract_arg("thread.cgroup", val, NULL);
	}
	else if(STR_MATCH("thread.cpu"))
	{
		if(alloc_state)
		{
			auto acc = m_inspector->m_thread_manager->dynamic_fields()->add_field<uint64_t>("_tmp_sinsp_filter_thread_cpu");
			m_thread_dyn_field_accessor.reset(new libsinsp::state::dynamic_struct::field_accessor<uint64_t>(acc.new_accessor<uint64_t>()));
		}

		return sinsp_filter_check::parse_field_name(str, alloc_state, needed_for_filtering);
	}
	else
	{
		return sinsp_filter_check::parse_field_name(str, alloc_state, needed_for_filtering);
	}
}

uint64_t sinsp_filter_check_thread::extract_exectime(sinsp_evt *evt)
{
	uint64_t res = 0;

	if(m_last_proc_switch_times.size() == 0)
	{
		//
		// Initialize the vector of CPU times
		//
		const scap_machine_info* minfo = m_inspector->get_machine_info();
		ASSERT(minfo->num_cpus != 0);

		for(uint32_t j = 0; j < minfo->num_cpus; j++)
		{
			m_last_proc_switch_times.push_back(0);
		}
	}

	uint32_t cpuid = evt->get_cpuid();
	uint64_t ts = evt->get_ts();
	uint64_t lasttime = m_last_proc_switch_times[cpuid];

	if(lasttime != 0)
	{
		res = ts - lasttime;
	}

	ASSERT(cpuid < m_last_proc_switch_times.size());

	m_last_proc_switch_times[cpuid] = ts;

	return res;
}

uint8_t* sinsp_filter_check_thread::extract_thread_cpu(sinsp_evt *evt, OUT uint32_t* len, sinsp_threadinfo* tinfo, bool extract_user, bool extract_system)
{
	uint16_t etype = evt->get_type();

	if(etype == PPME_PROCINFO_E)
	{
		uint64_t user = 0;
		uint64_t system = 0;
		uint64_t tcpu;

		if(extract_user)
		{
			sinsp_evt_param* parinfo = evt->get_param(0);
			user = *(uint64_t*)parinfo->m_val;
		}

		if(extract_system)
		{
			sinsp_evt_param* parinfo = evt->get_param(1);
			system = *(uint64_t*)parinfo->m_val;
		}

		tcpu = user + system;

		uint64_t last_t_tot_cpu = 0;
		tinfo->get_dynamic_field(*m_thread_dyn_field_accessor.get(), last_t_tot_cpu);
		if(last_t_tot_cpu != 0)
		{
			uint64_t deltaval = tcpu - last_t_tot_cpu;
			m_dval = (double)deltaval;// / (ONE_SECOND_IN_NS / 100);
			if(m_dval > 100)
			{
				m_dval = 100;
			}
		}
		else
		{
			m_dval = 0;
		}

		tinfo->set_dynamic_field(*m_thread_dyn_field_accessor.get(), tcpu);

		RETURN_EXTRACT_VAR(m_dval);
	}

	return NULL;
}

// Some syscall sources, such as the gVisor integration, cannot match events to host PIDs and TIDs.
// The event will retain the PID field which is consistent with the rest of sinsp logic, but it won't represent
// a real PID and so it should not be displayed to the user.
inline bool should_extract_xid(int64_t xid)
{
	return xid >= -1 && xid <= UINT32_MAX;
}

uint8_t* sinsp_filter_check_thread::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	sinsp_threadinfo* tinfo = evt->get_thread_info();

	if(tinfo == NULL &&
		m_field_id != TYPE_TID &&
		m_field_id != TYPE_EXECTIME &&
		m_field_id != TYPE_TOTEXECTIME)
	{
		return NULL;
	}

	switch(m_field_id)
	{
	case TYPE_TID:
		m_s64val = evt->get_tid();
		if (!should_extract_xid(m_s64val))
		{
			return NULL;
		}
		RETURN_EXTRACT_VAR(m_s64val);
	case TYPE_PID:
		if (!should_extract_xid(tinfo->m_pid))
		{
			return NULL;
		}
		RETURN_EXTRACT_VAR(tinfo->m_pid);
	case TYPE_SID:
		RETURN_EXTRACT_VAR(tinfo->m_sid);
	case TYPE_VPGID:
		RETURN_EXTRACT_VAR(tinfo->m_vpgid);
	case TYPE_SNAME:
		{
			int64_t sid = tinfo->m_sid;

			if(!tinfo->is_in_pid_namespace())
			{
				// Relying on the convention that a session id is the process id of the session leader.
				// `threadinfo` lookup only applies when the process is running on the host and not in a pid
				// namespace. However, if the process is running in a pid namespace, we instead traverse the process
				// lineage until we find a match.
				sinsp_threadinfo* sinfo = m_inspector->get_thread_ref(sid, false, true).get();
				if(sinfo != NULL)
				{
					m_tstr = sinfo->get_comm();
					RETURN_EXTRACT_STRING(m_tstr);
				}
			}

			// This can occur when the session leader process has exited or if the process
			// is running in a pid namespace and we only have the virtual session id, as 
			// seen from its pid namespace.
			// Find the highest ancestor process that has the same session id and
			// declare it to be the session leader.
			sinsp_threadinfo* session_leader = tinfo;

			sinsp_threadinfo::visitor_func_t visitor = [sid, &session_leader](sinsp_threadinfo* pt)
			{
				if(pt->m_sid != sid)
				{
					return false;
				}
				session_leader = pt;
				return true;
			};

			tinfo->traverse_parent_state(visitor);

			// session_leader has been updated to the highest process that has the same session id.
			// session_leader's comm is considered the session leader.
			m_tstr = session_leader->get_comm();
			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_SID_EXE:
		{
			int64_t sid = tinfo->m_sid;

			if(!tinfo->is_in_pid_namespace())
			{
				// Relying on the convention that a session id is the process id of the session leader.
				// `threadinfo` lookup only applies when the process is running on the host and not in a pid
				// namespace. However, if the process is running in a pid namespace, we instead traverse the process
				// lineage until we find a match.
				sinsp_threadinfo* sinfo = m_inspector->get_thread_ref(sid, false, true).get();
				if(sinfo != NULL)
				{
					m_tstr = sinfo->get_exe();
					RETURN_EXTRACT_STRING(m_tstr);
				}
			}

			// This can occur when the session leader process has exited or if the process
			// is running in a pid namespace and we only have the virtual session id, as 
			// seen from its pid namespace.
			// Find the highest ancestor process that has the same session id and
			// declare it to be the session leader.
			sinsp_threadinfo* session_leader = tinfo;

			sinsp_threadinfo::visitor_func_t visitor = [sid, &session_leader](sinsp_threadinfo* pt)
			{
				if(pt->m_sid != sid)
				{
					return false;
				}
				session_leader = pt;
				return true;
			};

			tinfo->traverse_parent_state(visitor);

			// session_leader has been updated to the highest process that has the same session id.
			// session_leader's exe is considered the session leader.
			m_tstr = session_leader->get_exe();
			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_SID_EXEPATH:
		{
			int64_t sid = tinfo->m_sid;

			if(!tinfo->is_in_pid_namespace())
			{
				// Relying on the convention that a session id is the process id of the session leader.
				// `threadinfo` lookup only applies when the process is running on the host and not in a pid
				// namespace. However, if the process is running in a pid namespace, we instead traverse the process
				// lineage until we find a match.
				sinsp_threadinfo* sinfo = m_inspector->get_thread_ref(sid, false, true).get();
				if(sinfo != NULL)
				{
					m_tstr = sinfo->get_exepath();
					RETURN_EXTRACT_STRING(m_tstr);
				}
			}
			
			// This can occur when the session leader process has exited or if the process
			// is running in a pid namespace and we only have the virtual session id, as 
			// seen from its pid namespace.
			// Find the highest ancestor process that has the same session id and
			// declare it to be the session leader.
			sinsp_threadinfo* session_leader = tinfo;

			sinsp_threadinfo::visitor_func_t visitor = [sid, &session_leader](sinsp_threadinfo* pt)
			{
				if(pt->m_sid != sid)
				{
					return false;
				}
				session_leader = pt;
				return true;
			};

			tinfo->traverse_parent_state(visitor);

			// session_leader has been updated to the highest process that has the same session id.
			// session_leader's exepath is considered the session leader.
			m_tstr = session_leader->get_exepath();
			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_VPGID_NAME:
		{
			int64_t vpgid = tinfo->m_vpgid;

			if(!tinfo->is_in_pid_namespace())
			{
				// Relying on the convention that a process group id is the process id of the process group leader.
				// `threadinfo` lookup only applies when the process is running on the host and not in a pid
				// namespace. However, if the process is running in a pid namespace, we instead traverse the process
				// lineage until we find a match.
				sinsp_threadinfo* vpgidinfo = m_inspector->get_thread_ref(vpgid, false, true).get();
				if(vpgidinfo != NULL)
				{
					m_tstr = vpgidinfo->get_comm();
					RETURN_EXTRACT_STRING(m_tstr);
				}
			}
			// This can occur when the process group leader process has exited or if the process
			// is running in a pid namespace and we only have the virtual process group id, as 
			// seen from its pid namespace.
			// Find the highest ancestor process that has the same process group id and
			// declare it to be the process group leader.
			sinsp_threadinfo* group_leader = tinfo;

			sinsp_threadinfo::visitor_func_t visitor = [vpgid, &group_leader](sinsp_threadinfo* pt)
			{
				if(pt->m_vpgid != vpgid)
				{
					return false;
				}
				group_leader = pt;
				return true;
			};

			tinfo->traverse_parent_state(visitor);

			// group_leader has been updated to the highest process that has the same process group id.
			// group_leader's comm is considered the process group leader.
			m_tstr = group_leader->get_comm();
			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_VPGID_EXE:
		{
			int64_t vpgid = tinfo->m_vpgid;

			if(!tinfo->is_in_pid_namespace())
			{
				// Relying on the convention that a process group id is the process id of the process group leader.
				// `threadinfo` lookup only applies when the process is running on the host and not in a pid
				// namespace. However, if the process is running in a pid namespace, we instead traverse the process
				// lineage until we find a match.
				sinsp_threadinfo* vpgidinfo = m_inspector->get_thread_ref(vpgid, false, true).get();
				if(vpgidinfo != NULL)
				{
					m_tstr = vpgidinfo->get_exe();
					RETURN_EXTRACT_STRING(m_tstr);
				}
			}
			// This can occur when the process group leader process has exited or if the process
			// is running in a pid namespace and we only have the virtual process group id, as 
			// seen from its pid namespace.
			// Find the highest ancestor process that has the same process group id and
			// declare it to be the process group leader.
			sinsp_threadinfo* group_leader = tinfo;

			sinsp_threadinfo::visitor_func_t visitor = [vpgid, &group_leader](sinsp_threadinfo* pt)
			{
				if(pt->m_vpgid != vpgid)
				{
					return false;
				}
				group_leader = pt;
				return true;
			};

			tinfo->traverse_parent_state(visitor);

			// group_leader has been updated to the highest process that has the same process group id.
			// group_leader's exe is considered the process group leader.
			m_tstr = group_leader->get_exe();
			RETURN_EXTRACT_STRING(m_tstr);

		}
	case TYPE_VPGID_EXEPATH:
		{
			int64_t vpgid = tinfo->m_vpgid;

			if(!tinfo->is_in_pid_namespace())
			{
				// Relying on the convention that a process group id is the process id of the process group leader.
				// `threadinfo` lookup only applies when the process is running on the host and not in a pid
				// namespace. However, if the process is running in a pid namespace, we instead traverse the process
				// lineage until we find a match.
				sinsp_threadinfo* vpgidinfo = m_inspector->get_thread_ref(vpgid, false, true).get();
				if(vpgidinfo != NULL)
				{
					m_tstr = vpgidinfo->get_exepath();
					RETURN_EXTRACT_STRING(m_tstr);
				}
			}

			// This can occur when the process group leader process has exited or if the process
			// is running in a pid namespace and we only have the virtual process group id, as 
			// seen from its pid namespace.
			// Find the highest ancestor process that has the same process group id and
			// declare it to be the process group leader.
			sinsp_threadinfo* group_leader = tinfo;

			sinsp_threadinfo::visitor_func_t visitor = [vpgid, &group_leader](sinsp_threadinfo* pt)
			{
				if(pt->m_vpgid != vpgid)
				{
					return false;
				}
				group_leader = pt;
				return true;
			};

			tinfo->traverse_parent_state(visitor);

			// group_leader has been updated to the highest process that has the same process group id.
			// group_leader's exepath is considered the process group leader.
			m_tstr = group_leader->get_exepath();
			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_TTY:
		RETURN_EXTRACT_VAR(tinfo->m_tty);
	case TYPE_NAME:
		m_tstr = tinfo->get_comm();
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_EXE:
		m_tstr = tinfo->get_exe();
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_EXEPATH:
		m_tstr = tinfo->get_exepath();
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_ARGS:
		{
			m_tstr.clear();

			uint32_t j;
			uint32_t nargs = (uint32_t)tinfo->m_args.size();

			for(j = 0; j < nargs; j++)
			{
				m_tstr += tinfo->m_args[j];
				if(j < nargs -1)
				{
					m_tstr += ' ';
				}
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_ENV:
		{
			m_tstr.clear();

			uint32_t j;
			const auto& env = tinfo->get_env();
			uint32_t nargs = (uint32_t)env.size();

			for(j = 0; j < nargs; j++)
			{
				m_tstr += env[j];
				if(j < nargs -1)
				{
					m_tstr += ' ';
				}
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_CMDLINE:
		{
			sinsp_threadinfo::populate_cmdline(m_tstr, tinfo);
			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_EXELINE:
		{
			m_tstr = tinfo->get_exe() + " ";

			uint32_t j;
			uint32_t nargs = (uint32_t)tinfo->m_args.size();

			for(j = 0; j < nargs; j++)
			{
				m_tstr += tinfo->m_args[j];
				if(j < nargs -1)
				{
					m_tstr += ' ';
				}
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_CWD:
		m_tstr = tinfo->get_cwd();
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_NTHREADS:
		{
			m_u64val = tinfo->get_num_threads();
			RETURN_EXTRACT_VAR(m_u64val);
		}
		break;
	case TYPE_NCHILDS:
		{
			m_u64val = tinfo->get_num_not_leader_threads();
			RETURN_EXTRACT_VAR(m_u64val);
		}
		break;
	case TYPE_ISMAINTHREAD:
		m_tbool = (uint32_t)tinfo->is_main_thread();
		RETURN_EXTRACT_VAR(m_tbool);
	case TYPE_EXECTIME:
		{
			m_u64val = 0;
			uint16_t etype = evt->get_type();

			if(etype == PPME_SCHEDSWITCH_1_E || etype == PPME_SCHEDSWITCH_6_E)
			{
				m_u64val = extract_exectime(evt);
			}

			RETURN_EXTRACT_VAR(m_u64val);
		}
	case TYPE_TOTEXECTIME:
		{
			m_u64val = 0;
			uint16_t etype = evt->get_type();

			if(etype == PPME_SCHEDSWITCH_1_E || etype == PPME_SCHEDSWITCH_6_E)
			{
				m_u64val = extract_exectime(evt);
			}

			sinsp_threadinfo* tinfo = evt->get_thread_info(false);

			if(tinfo != NULL)
			{
				uint64_t ptot = 0;
				tinfo->get_dynamic_field(*m_thread_dyn_field_accessor.get(), ptot);
				m_u64val += ptot;
				tinfo->set_dynamic_field(*m_thread_dyn_field_accessor.get(), m_u64val);
				RETURN_EXTRACT_VAR(m_u64val);
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_PPID:
		if(tinfo->is_main_thread())
		{
			if (!should_extract_xid(tinfo->m_ptid))
			{
				return NULL;
			}
			RETURN_EXTRACT_VAR(tinfo->m_ptid);
		}
		else
		{
			sinsp_threadinfo* mt = tinfo->get_main_thread();

			if(mt != NULL)
			{
				if (!should_extract_xid(mt->m_ptid))
				{
					return NULL;
				}
				RETURN_EXTRACT_VAR(mt->m_ptid);
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_PNAME:
		{
			sinsp_threadinfo* ptinfo =
				m_inspector->get_thread_ref(tinfo->m_ptid, false, true).get();

			if(ptinfo != NULL)
			{
				m_tstr = ptinfo->get_comm();
				RETURN_EXTRACT_STRING(m_tstr);
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_PCMDLINE:
		{
			sinsp_threadinfo* ptinfo =
				m_inspector->get_thread_ref(tinfo->m_ptid, false, true).get();

			if(ptinfo != NULL)
			{
				sinsp_threadinfo::populate_cmdline(m_tstr, ptinfo);
				RETURN_EXTRACT_STRING(m_tstr);
			}
			else
			{
				return NULL;
			}
		}
		case TYPE_ACMDLINE:
		{
			sinsp_threadinfo* mt = NULL;

			if(tinfo->is_main_thread())
			{
				mt = tinfo;
			}
			else
			{
				mt = tinfo->get_main_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			for(int32_t j = 0; j < m_argid; j++)
			{
				mt = mt->get_parent_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}
			sinsp_threadinfo::populate_cmdline(m_tstr, mt);
			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_APID:
		{
			sinsp_threadinfo* mt = NULL;

			if(tinfo->is_main_thread())
			{
				mt = tinfo;
			}
			else
			{
				mt = tinfo->get_main_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			//
			// Search for a specific ancestors
			//
			for(int32_t j = 0; j < m_argid; j++)
			{
				mt = mt->get_parent_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			if (!should_extract_xid(mt->m_pid))
			{
				return NULL;
			}
			RETURN_EXTRACT_VAR(mt->m_pid);
		}
	case TYPE_ANAME:
		{
			sinsp_threadinfo* mt = NULL;

			if(tinfo->is_main_thread())
			{
				mt = tinfo;
			}
			else
			{
				mt = tinfo->get_main_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			for(int32_t j = 0; j < m_argid; j++)
			{
				mt = mt->get_parent_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			m_tstr = mt->get_comm();
			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_PEXE:
		{
			sinsp_threadinfo* ptinfo =
				m_inspector->get_thread_ref(tinfo->m_ptid, false, true).get();

			if(ptinfo != NULL)
			{
				m_tstr = ptinfo->get_exe();
				RETURN_EXTRACT_STRING(m_tstr);
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_AEXE:
		{
			sinsp_threadinfo* mt = NULL;

			if(tinfo->is_main_thread())
			{
				mt = tinfo;
			}
			else
			{
				mt = tinfo->get_main_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			for(int32_t j = 0; j < m_argid; j++)
			{
				mt = mt->get_parent_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			m_tstr = mt->get_exe();
			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_PEXEPATH:
		{
			sinsp_threadinfo* ptinfo =
				m_inspector->get_thread_ref(tinfo->m_ptid, false, true).get();

			if(ptinfo != NULL)
			{
				m_tstr = ptinfo->get_exepath();
				RETURN_EXTRACT_STRING(m_tstr);
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_AEXEPATH:
		{
			sinsp_threadinfo* mt = NULL;

			if(tinfo->is_main_thread())
			{
				mt = tinfo;
			}
			else
			{
				mt = tinfo->get_main_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			for(int32_t j = 0; j < m_argid; j++)
			{
				mt = mt->get_parent_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			m_tstr = mt->get_exepath();
			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_LOGINSHELLID:
		{
			sinsp_threadinfo* mt = NULL;
			int64_t* res = NULL;

			if(tinfo->is_main_thread())
			{
				mt = tinfo;
			}
			else
			{
				mt = tinfo->get_main_thread();

				if(mt == NULL)
				{
					return NULL;
				}
			}

			sinsp_threadinfo::visitor_func_t check_thread_for_shell = [&res] (sinsp_threadinfo *pt)
			{
				size_t len = pt->m_comm.size();

				if(len >= 2 && pt->m_comm[len - 2] == 's' && pt->m_comm[len - 1] == 'h')
				{
					res = &pt->m_pid;
				}

				return true;
			};

			// First call the visitor on the main thread.
			check_thread_for_shell(mt);

			// Then check all its parents to see if they are shells
			mt->traverse_parent_state(check_thread_for_shell);

			RETURN_EXTRACT_PTR(res);
		}
	case TYPE_DURATION:
		if(tinfo->m_clone_ts != 0)
		{
			m_s64val = evt->get_ts() - tinfo->m_clone_ts;
			ASSERT(m_s64val > 0);
			RETURN_EXTRACT_VAR(m_s64val);
		}
		else
		{
			return NULL;
		}
	case TYPE_PPID_DURATION:
		{
			sinsp_threadinfo* ptinfo =
				m_inspector->get_thread_ref(tinfo->m_ptid, false, true).get();

			if(ptinfo != NULL)
			{
				if(ptinfo->m_clone_ts != 0)
				{
					m_s64val = evt->get_ts() - ptinfo->m_clone_ts;
					ASSERT(m_s64val > 0);
					RETURN_EXTRACT_VAR(m_s64val);
				}
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_FDOPENCOUNT:
		m_u64val = tinfo->get_fd_opencount();
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_FDLIMIT:
		m_s64val = tinfo->get_fd_limit();
		RETURN_EXTRACT_VAR(m_s64val);
	case TYPE_FDUSAGE:
		m_dval = tinfo->get_fd_usage_pct_d();
		RETURN_EXTRACT_VAR(m_dval);
	case TYPE_VMSIZE:
		m_u64val = tinfo->m_vmsize_kb;
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_VMRSS:
		m_u64val = tinfo->m_vmrss_kb;
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_VMSWAP:
		m_u64val = tinfo->m_vmswap_kb;
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_THREAD_VMSIZE:
		if(tinfo->is_main_thread())
		{
			m_u64val = tinfo->m_vmsize_kb;
		}
		else
		{
			m_u64val = 0;
		}

		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_THREAD_VMRSS:
		if(tinfo->is_main_thread())
		{
			m_u64val = tinfo->m_vmrss_kb;
		}
		else
		{
			m_u64val = 0;
		}

		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_THREAD_VMSIZE_B:
		if(tinfo->is_main_thread())
		{
			m_u64val = tinfo->m_vmsize_kb * 1024;
		}
		else
		{
			m_u64val = 0;
		}

		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_THREAD_VMRSS_B:
		if(tinfo->is_main_thread())
		{
			m_u64val = tinfo->m_vmrss_kb * 1024;
		}
		else
		{
			m_u64val = 0;
		}

		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_PFMAJOR:
		m_u64val = tinfo->m_pfmajor;
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_PFMINOR:
		m_u64val = tinfo->m_pfminor;
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_CGROUPS:
		{
			m_tstr.clear();
			auto cgroups = tinfo->cgroups();

			uint32_t j;
			uint32_t nargs = (uint32_t)cgroups.size();

			if(nargs == 0)
			{
				return NULL;
			}

			for(j = 0; j < nargs; j++)
			{
				m_tstr += cgroups[j].first;
				m_tstr += "=";
				m_tstr += cgroups[j].second;
				if(j < nargs - 1)
				{
					m_tstr += ' ';
				}
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
	case TYPE_CGROUP:
		if(tinfo->get_cgroup(m_argname, m_tstr))
		{
			RETURN_EXTRACT_STRING(m_tstr);
		}
		return NULL;
	case TYPE_VTID:
		if(tinfo->m_vtid == -1)
		{
			return NULL;
		}

		m_u64val = tinfo->m_vtid;
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_VPID:
		if(tinfo->m_vpid == -1)
		{
			return NULL;
		}

		m_u64val = tinfo->m_vpid;
		RETURN_EXTRACT_VAR(m_u64val);
/*
	case TYPE_PROC_CPU:
		{
			uint16_t etype = evt->get_type();

			if(etype == PPME_PROCINFO_E)
			{
				double thval;
				uint64_t tcpu;

				sinsp_evt_param* parinfo = evt->get_param(0);
				tcpu = *(uint64_t*)parinfo->m_val;

				parinfo = evt->get_param(1);
				tcpu += *(uint64_t*)parinfo->m_val;

				if(tinfo->m_last_t_tot_cpu != 0)
				{
					uint64_t deltaval = tcpu - tinfo->m_last_t_tot_cpu;
					thval = (double)deltaval;// / (ONE_SECOND_IN_NS / 100);
					if(thval > 100)
					{
						thval = 100;
					}
				}
				else
				{
					thval = 0;
				}

				tinfo->m_last_t_tot_cpu = tcpu;

				uint64_t ets = evt->get_ts();
				sinsp_threadinfo* mt = tinfo->get_main_thread();

				if(ets != mt->m_last_mt_cpu_ts)
				{
					mt->m_last_mt_tot_cpu = 0;
					mt->m_last_mt_cpu_ts = ets;
				}

				mt->m_last_mt_tot_cpu += thval;
				m_dval = mt->m_last_mt_tot_cpu;

				RETURN_EXTRACT_VAR(m_dval);
			}

			return NULL;
		}
*/
	case TYPE_THREAD_CPU:
		{
			return extract_thread_cpu(evt, len, tinfo, true, true);
		}
	case TYPE_THREAD_CPU_USER:
		{
			return extract_thread_cpu(evt, len, tinfo, true, false);
		}
	case TYPE_THREAD_CPU_SYSTEM:
		{
			return extract_thread_cpu(evt, len, tinfo, false, true);
		}
	case TYPE_NAMETID:
		m_tstr = tinfo->get_comm() + to_string(evt->get_tid());
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_IS_CONTAINER_HEALTHCHECK:
		m_tbool = (tinfo->m_category == sinsp_threadinfo::CAT_HEALTHCHECK);
		RETURN_EXTRACT_VAR(m_tbool);
	case TYPE_IS_CONTAINER_LIVENESS_PROBE:
		m_tbool = (tinfo->m_category == sinsp_threadinfo::CAT_LIVENESS_PROBE);
		RETURN_EXTRACT_VAR(m_tbool);
	case TYPE_IS_CONTAINER_READINESS_PROBE:
		m_tbool = (tinfo->m_category == sinsp_threadinfo::CAT_READINESS_PROBE);
		RETURN_EXTRACT_VAR(m_tbool);
	case TYPE_IS_EXE_WRITABLE:
		m_tbool = tinfo->m_exe_writable;
		RETURN_EXTRACT_VAR(m_tbool);
	case TYPE_IS_EXE_UPPER_LAYER:
		m_tbool = tinfo->m_exe_upper_layer;
		RETURN_EXTRACT_VAR(m_tbool);
	case TYPE_IS_EXE_FROM_MEMFD:
		m_tbool = tinfo->m_exe_from_memfd;
		RETURN_EXTRACT_VAR(m_tbool);
	case TYPE_IS_SID_LEADER:
		m_tbool = tinfo->m_sid == tinfo->m_vpid;
		RETURN_EXTRACT_VAR(m_tbool);
	case TYPE_IS_VPGID_LEADER:
		m_tbool = tinfo->m_vpgid == tinfo->m_vpid;
		RETURN_EXTRACT_VAR(m_tbool);
	case TYPE_CAP_PERMITTED:
		m_tstr = sinsp_utils::caps_to_string(tinfo->m_cap_permitted);
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_CAP_INHERITABLE:
		m_tstr = sinsp_utils::caps_to_string(tinfo->m_cap_inheritable);
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_CAP_EFFECTIVE:
		m_tstr = sinsp_utils::caps_to_string(tinfo->m_cap_effective);
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_CMDNARGS:
		{
			m_u64val = (uint32_t)tinfo->m_args.size();
			RETURN_EXTRACT_VAR(m_u64val);
		}
	case TYPE_CMDLENARGS:
		{
			m_u64val = 0;
			uint32_t j;
			uint32_t nargs = (uint32_t)tinfo->m_args.size();

			for(j = 0; j < nargs; j++)
			{
				m_u64val += tinfo->m_args[j].length();

			}
			RETURN_EXTRACT_VAR(m_u64val);
		}
	case TYPE_PVPID:
		{
			sinsp_threadinfo* ptinfo =
				m_inspector->get_thread_ref(tinfo->m_ptid, false, true).get();

			if(ptinfo != NULL)
			{
				RETURN_EXTRACT_VAR(ptinfo->m_vpid);
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_EXE_INO:
		// Inode 0 is used as a NULL value to indicate that there is no inode.
		if(tinfo->m_exe_ino == 0)
		{
			return NULL;
		}
		RETURN_EXTRACT_VAR(tinfo->m_exe_ino);
	case TYPE_EXE_INO_CTIME:
		if(tinfo->m_exe_ino_ctime == 0)
		{
			return NULL;
		}
		RETURN_EXTRACT_VAR(tinfo->m_exe_ino_ctime);
	case TYPE_EXE_INO_MTIME:
		if(tinfo->m_exe_ino_mtime == 0)
		{
			return NULL;
		}
		RETURN_EXTRACT_VAR(tinfo->m_exe_ino_mtime);
	case TYPE_EXE_INO_CTIME_DURATION_CLONE_TS:
		if(tinfo->m_exe_ino_ctime_duration_clone_ts == 0)
		{
			return NULL;
		}
		RETURN_EXTRACT_VAR(tinfo->m_exe_ino_ctime_duration_clone_ts);
	case TYPE_EXE_INO_CTIME_DURATION_PIDNS_START:
		if(tinfo->m_exe_ino_ctime_duration_pidns_start == 0)
		{
			return NULL;
		}
		RETURN_EXTRACT_VAR(tinfo->m_exe_ino_ctime_duration_pidns_start);
	case TYPE_PIDNS_INIT_START_TS:
		if(tinfo->m_pidns_init_start_ts == 0)
		{
			return NULL;
		}
		RETURN_EXTRACT_VAR(tinfo->m_pidns_init_start_ts);
	case TYPE_PID_CLONE_TS:
		if(tinfo->m_clone_ts == 0)
		{
			return NULL;
		}
		RETURN_EXTRACT_VAR(tinfo->m_clone_ts);
	case TYPE_PPID_CLONE_TS:
		{
			sinsp_threadinfo* ptinfo =
				m_inspector->get_thread_ref(tinfo->m_ptid, false, true).get();

			if(ptinfo != NULL)
			{
				RETURN_EXTRACT_VAR(ptinfo->m_clone_ts);
			}
			else
			{
				return NULL;
			}
		}
	default:
		ASSERT(false);
		return NULL;
	}
}

bool sinsp_filter_check_thread::compare_full_apid(sinsp_evt *evt)
{
	sinsp_threadinfo* tinfo = evt->get_thread_info();
	if(tinfo == NULL)
	{
		return false;
	}

	sinsp_threadinfo* mt = NULL;

	if(tinfo->is_main_thread())
	{
		mt = tinfo;
	}
	else
	{
		mt = tinfo->get_main_thread();

		if(mt == NULL)
		{
			return false;
		}
	}

	//
	// No id specified, search in all of the ancestors
	//
	bool found = false;
	sinsp_threadinfo::visitor_func_t visitor = [this, &found] (sinsp_threadinfo *pt)
	{
		bool res;
		res = flt_compare(m_cmpop,
				  PT_PID,
				  &pt->m_pid);

		if(res == true)
		{
			found = true;

			// Can stop traversing parent state
			return false;
		}

		return true;
	};

	mt->traverse_parent_state(visitor);

	return found;
}

bool sinsp_filter_check_thread::compare_full_aname(sinsp_evt *evt)
{
	sinsp_threadinfo* tinfo = evt->get_thread_info();

	if(tinfo == NULL)
	{
		return false;
	}

	sinsp_threadinfo* mt = NULL;

	if(tinfo->is_main_thread())
	{
		mt = tinfo;
	}
	else
	{
		mt = tinfo->get_main_thread();

		if(mt == NULL)
		{
			return false;
		}
	}

	//
	// No id specified, search in all of the ancestors
	//
	bool found = false;
	sinsp_threadinfo::visitor_func_t visitor = [this, &found] (sinsp_threadinfo *pt)
	{
		bool res;

		res = flt_compare(m_cmpop,
				  PT_CHARBUF,
				  (void*)pt->m_comm.c_str());

		if(res == true)
		{
			found = true;

			// Can stop traversing parent state
			return false;
		}

		return true;
	};

	mt->traverse_parent_state(visitor);

	return found;
}

bool sinsp_filter_check_thread::compare_full_aexe(sinsp_evt *evt)
{
	sinsp_threadinfo* tinfo = evt->get_thread_info();

	if(tinfo == NULL)
	{
		return false;
	}

	sinsp_threadinfo* mt = NULL;

	if(tinfo->is_main_thread())
	{
		mt = tinfo;
	}
	else
	{
		mt = tinfo->get_main_thread();

		if(mt == NULL)
		{
			return false;
		}
	}

	//
	// No id specified, search in all of the ancestors
	//
	bool found = false;
	sinsp_threadinfo::visitor_func_t visitor = [this, &found] (sinsp_threadinfo *pt)
	{
		bool res;

		res = flt_compare(m_cmpop,
				  PT_CHARBUF,
				  (void*)pt->m_exe.c_str());

		if(res == true)
		{
			found = true;

			// Can stop traversing parent state
			return false;
		}

		return true;
	};

	mt->traverse_parent_state(visitor);

	return found;
}

bool sinsp_filter_check_thread::compare_full_aexepath(sinsp_evt *evt)
{
	sinsp_threadinfo* tinfo = evt->get_thread_info();

	if(tinfo == NULL)
	{
		return false;
	}

	sinsp_threadinfo* mt = NULL;

	if(tinfo->is_main_thread())
	{
		mt = tinfo;
	}
	else
	{
		mt = tinfo->get_main_thread();

		if(mt == NULL)
		{
			return false;
		}
	}

	//
	// No id specified, search in all of the ancestors
	//
	bool found = false;
	sinsp_threadinfo::visitor_func_t visitor = [this, &found] (sinsp_threadinfo *pt)
	{
		bool res;

		res = flt_compare(m_cmpop,
				  PT_CHARBUF,
				  (void*)pt->m_exepath.c_str());

		if(res == true)
		{
			found = true;

			// Can stop traversing parent state
			return false;
		}

		return true;
	};

	mt->traverse_parent_state(visitor);

	return found;
}

bool sinsp_filter_check_thread::compare_full_acmdline(sinsp_evt *evt)
{
	sinsp_threadinfo* tinfo = evt->get_thread_info();

	if(tinfo == NULL)
	{
		return false;
	}

	sinsp_threadinfo* mt = NULL;

	if(tinfo->is_main_thread())
	{
		mt = tinfo;
	}
	else
	{
		mt = tinfo->get_main_thread();

		if(mt == NULL)
		{
			return false;
		}
	}

	//
	// No id specified, search in all of the ancestors
	//
	bool found = false;
	sinsp_threadinfo::visitor_func_t visitor = [this, &found] (sinsp_threadinfo *pt)
	{
		bool res;
		std::string cmdline;
		sinsp_threadinfo::populate_cmdline(cmdline, pt);

		res = flt_compare(m_cmpop,
				  PT_CHARBUF,
				  (void*)cmdline.c_str());

		if(res == true)
		{
			found = true;

			// Can stop traversing parent state
			return false;
		}

		return true;
	};

	mt->traverse_parent_state(visitor);

	return found;
}

bool sinsp_filter_check_thread::compare(sinsp_evt *evt)
{
	if(m_field_id == TYPE_APID)
	{
		if(m_argid == -1)
		{
			// return filter_proc(evt);
			return compare_full_apid(evt);
		}
	}
	else if(m_field_id == TYPE_ANAME)
	{
		if(m_argid == -1)
		{
			return compare_full_aname(evt);
		}
	}
	else if(m_field_id == TYPE_AEXE)
	{
		if(m_argid == -1)
		{
			return compare_full_aexe(evt);
		}
	}
	else if(m_field_id == TYPE_AEXEPATH)
	{
		if(m_argid == -1)
		{
			return compare_full_aexepath(evt);
		}
	}
	else if(m_field_id == TYPE_ACMDLINE)
	{
		if(m_argid == -1)
		{
			return compare_full_acmdline(evt);
		}
	}

	return sinsp_filter_check::compare(evt);
}

int32_t sinsp_filter_check_thread::get_argid()
{
	return m_argid;
}

bool sinsp_filter_check_thread::filter_proc(sinsp_evt *evt)
{
	sinsp_threadinfo* mt = evt->get_thread_info();
	if(!mt)
	{
		return false;
	}

	sinsp_threadinfo* pmt = NULL;
	if(mt->is_main_thread())
	{
		pmt = mt;
	}
	else
	{
		pmt = mt->get_parent_thread();

		if(pmt == NULL)
		{
			return false;
		}
	}

	if(m_proc_set.empty())
	{
		int64_t apid = *(int64_t *)filter_value_p();
		m_proc_set.insert(apid);
	}

	uint16_t evt_type =evt->get_type();
	int64_t svpid = 0;
	int64_t sres = 0;
	switch (evt_type)
	{
	case PPME_SYSCALL_CLONE_11_X:
	case PPME_SYSCALL_CLONE_16_X:
	case PPME_SYSCALL_CLONE_17_X:
	case PPME_SYSCALL_CLONE_20_X:
	case PPME_SYSCALL_CLONE3_X:
	case PPME_SYSCALL_FORK_X:
	case PPME_SYSCALL_FORK_17_X:
	case PPME_SYSCALL_FORK_20_X:
	case PPME_SYSCALL_VFORK_X:
	case PPME_SYSCALL_VFORK_17_X:
	case PPME_SYSCALL_VFORK_20_X:
		{
			for(uint32_t i=0; i < evt->get_num_params(); i++)
			{
				sinsp_evt_param * param = evt->get_param(i);
				if(strcmp(evt->get_param_name(i), "res") == 0)
				{
					sres = *(int64_t*)param->m_val;
				}
				if(strcmp(evt->get_param_name(i), "vpid") == 0)
				{
					svpid = *(int64_t*)param->m_val;
				}
			}

			// container evt
			if(svpid > 0)
			{
				// return value of clone as svpid
				if(sres > 0)
				{
					svpid = sres;
				}

				// svpid associative container
				int8_t c;
				for (auto ch : mt->m_container_id) 
				{
					c = ch;
					svpid = ((svpid << 5) + svpid) + c;
				}
			}
		}
		break;
	default:
		break;
	}

	if(m_proc_set.find(mt->m_pid) != m_proc_set.end())
	{
		if(svpid > 0)
		{
			m_proc_set.insert(svpid);
		}
		return true;
	}

	if(m_proc_set.find(pmt->m_pid) != m_proc_set.end() || compare_full_apid(evt))
	{
		m_proc_set.insert(mt->m_pid);
		if(svpid > 0)
		{
			m_proc_set.insert(svpid);
		}
		return true;
	}

	if(svpid > 0 && m_proc_set.find(svpid) != m_proc_set.end())
	{
		m_proc_set.insert(mt->m_pid);
		return true;
	}

	return false;
}


///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_event implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_gen_event_fields[] =
{
	{PT_UINT64, EPF_NONE, PF_ID, "evt.num", "Event Number", "event number."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.time", "Time", "event timestamp as a time string that includes the nanosecond part."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.time.s", "Time (s)", "event timestamp as a time string with no nanoseconds."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.time.iso8601", "ISO 8601 Time", "event timestamp in ISO 8601 format, including nanoseconds and time zone offset (in UTC)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.datetime", "Datetime", "event timestamp as a time string that includes the date."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.datetime.s", "Datetime (s)", "event timestamp as a datetime string with no nanoseconds."},
	{PT_ABSTIME, EPF_NONE, PF_DEC, "evt.rawtime", "Absolute Time", "absolute event timestamp, i.e. nanoseconds from epoch."},
	{PT_ABSTIME, EPF_NONE, PF_DEC, "evt.rawtime.s", "Absolute Time (s)", "integer part of the event timestamp (e.g. seconds since epoch)."},
	{PT_ABSTIME, EPF_NONE, PF_10_PADDED_DEC, "evt.rawtime.ns", "Absolute Time (ns)", "fractional part of the absolute event timestamp."},
	{PT_RELTIME, EPF_NONE, PF_10_PADDED_DEC, "evt.reltime", "Relative Time", "number of nanoseconds from the beginning of the capture."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "evt.reltime.s", "Relative Time (s)", "number of seconds from the beginning of the capture."},
	{PT_RELTIME, EPF_NONE, PF_10_PADDED_DEC, "evt.reltime.ns", "Relative Time (ns)", "fractional part (in ns) of the time from the beginning of the capture."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.pluginname", "Plugin Name", "if the event comes from a plugin-defined event source, the name of the plugin that generated it. The plugin must be currently loaded."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.plugininfo", "Plugin Info", "if the event comes from a plugin-defined event source, a summary of the event as formatted by the plugin. The plugin must be currently loaded."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.source", "Event Source", "the name of the source that produced the event."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_async", "Async Event", "'true' for asynchronous events, 'false' otherwise."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.asynctype", "Async-Event Type", "If the event is asynchronous, the type of the event (e.g. 'container')."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.hostname", "Hostname", "The hostname of the underlying host can be customized by setting an environment variable (e.g. FALCO_HOSTNAME for the Falco agent). This is valuable in Kubernetes setups, where the hostname can match the pod name particularly in DaemonSet deployments. To achieve this, assign Kubernetes' spec.nodeName to the environment variable. Notably, spec.nodeName generally includes the cluster name."},
	/* Note for libs adopters: libs exposes a customizable env variable for hostname which defaults to `set(SCAP_HOSTNAME_ENV_VAR "SCAP_HOSTNAME")`, and Falco client adopts "FALCO_HOSTNAME". */
};

sinsp_filter_check_gen_event::sinsp_filter_check_gen_event()
{
	m_info.m_name = "evt";
	m_info.m_shortdesc = "All event types";
	m_info.m_desc = "These fields can be used for all event types";
	m_info.m_fields = sinsp_filter_check_gen_event_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_gen_event_fields) / sizeof(sinsp_filter_check_gen_event_fields[0]);
	m_u64val = 0;
}

sinsp_filter_check_gen_event::~sinsp_filter_check_gen_event()
{
}

sinsp_filter_check* sinsp_filter_check_gen_event::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_gen_event();
}

Json::Value sinsp_filter_check_gen_event::extract_as_js(sinsp_evt *evt, OUT uint32_t* len)
{
	switch(m_field_id)
	{
	case TYPE_TIME:
	case TYPE_TIME_S:
	case TYPE_TIME_ISO8601:
	case TYPE_DATETIME:
	case TYPE_DATETIME_S:
		return (Json::Value::Int64)evt->get_ts();

	case TYPE_RAWTS:
	case TYPE_RAWTS_S:
	case TYPE_RAWTS_NS:
	case TYPE_RELTS:
	case TYPE_RELTS_S:
	case TYPE_RELTS_NS:
		return (Json::Value::Int64)*(uint64_t*)extract(evt, len);
	default:
		return Json::nullValue;
	}

	return Json::nullValue;
}

uint8_t* sinsp_filter_check_gen_event::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{

	std::shared_ptr<sinsp_plugin> plugin;
	const scap_machine_info* minfo;

	*len = 0;
	switch(m_field_id)
	{
	case TYPE_TIME:
		if(false)
		{
			m_strstorage = to_string(evt->get_ts());
		}
		else
		{
			sinsp_utils::ts_to_string(evt->get_ts(), &m_strstorage, false, true);
		}
		RETURN_EXTRACT_STRING(m_strstorage);
	case TYPE_TIME_S:
		sinsp_utils::ts_to_string(evt->get_ts(), &m_strstorage, false, false);
		RETURN_EXTRACT_STRING(m_strstorage);
	case TYPE_TIME_ISO8601:
		sinsp_utils::ts_to_iso_8601(evt->get_ts(), &m_strstorage);
		RETURN_EXTRACT_STRING(m_strstorage);
	case TYPE_DATETIME:
		sinsp_utils::ts_to_string(evt->get_ts(), &m_strstorage, true, true);
		RETURN_EXTRACT_STRING(m_strstorage);
	case TYPE_DATETIME_S:
		sinsp_utils::ts_to_string(evt->get_ts(), &m_strstorage, true, false);
		RETURN_EXTRACT_STRING(m_strstorage);
	case TYPE_RAWTS:
		m_u64val = evt->get_ts();
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_RAWTS_S:
		m_u64val = evt->get_ts() / ONE_SECOND_IN_NS;
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_RAWTS_NS:
		m_u64val = evt->get_ts() % ONE_SECOND_IN_NS;
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_RELTS:
		m_u64val = evt->get_ts() - m_inspector->m_firstevent_ts;
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_RELTS_S:
		m_u64val = (evt->get_ts() - m_inspector->m_firstevent_ts) / ONE_SECOND_IN_NS;
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_RELTS_NS:
		m_u64val = (evt->get_ts() - m_inspector->m_firstevent_ts) % ONE_SECOND_IN_NS;
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_NUMBER:
		m_u64val = evt->get_num();
		RETURN_EXTRACT_VAR(m_u64val);
	case TYPE_PLUGINNAME:
	case TYPE_PLUGININFO:
		plugin = m_inspector->get_plugin_manager()->plugin_by_evt(evt);
		if (plugin == nullptr)
		{
			return NULL;
		}

		if(m_field_id == TYPE_PLUGINNAME)
		{
			m_strstorage = plugin->name();
		}
		else
		{
			m_strstorage = plugin->event_to_string(evt);
		}

		RETURN_EXTRACT_STRING(m_strstorage);
	case TYPE_SOURCE:
		if (evt->get_source_idx() == sinsp_no_event_source_idx
			|| evt->get_source_name() == sinsp_no_event_source_name)
		{
			return NULL;
		}
		RETURN_EXTRACT_CSTR(evt->get_source_name());
	case TYPE_ISASYNC:
		if (libsinsp::events::is_metaevent((ppm_event_code) evt->get_type()))
		{
			m_u32val = 1;
		}
		else
		{
			m_u32val = 0;
		}
		RETURN_EXTRACT_VAR(m_u32val);
	case TYPE_ASYNCTYPE:
		if (!libsinsp::events::is_metaevent((ppm_event_code) evt->get_type()))
		{
			return NULL;
		}
		if (evt->get_type() == PPME_ASYNCEVENT_E)
		{
			RETURN_EXTRACT_CSTR(evt->get_param(1)->m_val);
		}
		RETURN_EXTRACT_CSTR(evt->get_name());
	case TYPE_HOSTNAME:
		minfo = m_inspector->get_machine_info();
		if (!minfo)
		{
			return NULL;
		}
		RETURN_EXTRACT_CSTR(minfo->hostname);
	default:
		ASSERT(false);
		return NULL;
	}

	return NULL;
}

const filtercheck_field_info sinsp_filter_check_event_fields[] =
{
	{PT_RELTIME, EPF_NONE, PF_DEC, "evt.latency", "Latency", "delta between an exit event and the correspondent enter event, in nanoseconds."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "evt.latency.s", "Latency (s)", "integer part of the event latency delta."},
	{PT_RELTIME, EPF_NONE, PF_10_PADDED_DEC, "evt.latency.ns", "Latency (ns)", "fractional part of the event latency delta."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.latency.quantized", "Quantized Latency", "10-base log of the delta between an exit event and the correspondent enter event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.latency.human", "Human-Readable Latency", "delta between an exit event and the correspondent enter event, as a human readable string (e.g. 10.3ms)."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "evt.deltatime", "Delta", "delta between this event and the previous event, in nanoseconds."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "evt.deltatime.s", "Delta (s)", "integer part of the delta between this event and the previous event."},
	{PT_RELTIME, EPF_NONE, PF_10_PADDED_DEC, "evt.deltatime.ns", "Delta (ns)", "fractional part of the delta between this event and the previous event."},
	{PT_CHARBUF, EPF_PRINT_ONLY, PF_NA, "evt.outputtime", "Output Time", "this depends on -t param, default is %evt.time ('h')."},
	{PT_CHARBUF, EPF_NONE, PF_DIR, "evt.dir", "Direction", "event direction can be either '>' for enter events or '<' for exit events."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.type", "Type", "The name of the event (e.g. 'open')."},
	{PT_UINT32, EPF_ARG_REQUIRED, PF_NA, "evt.type.is", "Type Is", "allows one to specify an event type, and returns 1 for events that are of that type. For example, evt.type.is.open returns 1 for open events, 0 for any other event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "syscall.type", "Syscall Type", "For system call events, the name of the system call (e.g. 'open'). Unset for other events (e.g. switch or internal events). Use this field instead of evt.type if you need to make sure that the filtered/printed value is actually a system call."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.category", "Category", "The event category. Example values are 'file' (for file operations like open and close), 'net' (for network operations like socket and bind), memory (for things like brk or mmap), and so on."},
	{PT_INT16, EPF_NONE, PF_ID, "evt.cpu", "CPU Number", "number of the CPU where this event happened."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.args", "Arguments", "all the event arguments, aggregated into a single string."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "evt.arg", "Argument", "one of the event arguments specified by name or by number. Some events (e.g. return codes or FDs) will be converted into a text representation when possible. E.g. 'evt.arg.fd' or 'evt.arg[0]'."},
	{PT_DYN, EPF_ARG_REQUIRED, PF_NA, "evt.rawarg", "Raw Argument", "one of the event arguments specified by name. E.g. 'evt.rawarg.fd'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.info", "Information", "for most events, this field returns the same value as evt.args. However, for some events (like writes to /dev/log) it provides higher level information coming from decoding the arguments."},
	{PT_BYTEBUF, EPF_NONE, PF_NA, "evt.buffer", "Buffer", "the binary data buffer for events that have one, like read(), recvfrom(), etc. Use this field in filters with 'contains' to search into I/O data buffers."},
	{PT_UINT64, EPF_NONE, PF_DEC, "evt.buflen", "Buffer Length", "the length of the binary data buffer for events that have one, like read(), recvfrom(), etc."},
	{PT_CHARBUF, EPF_NONE, PF_DEC, "evt.res", "Return Value", "event return value, as a string. If the event failed, the result is an error code string (e.g. 'ENOENT'), otherwise the result is the string 'SUCCESS'."},
	{PT_INT64, EPF_NONE, PF_DEC, "evt.rawres", "Raw Return Value", "event return value, as a number (e.g. -2). Useful for range comparisons."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.failed", "Failed", "'true' for events that returned an error status."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_io", "Is I/O", "'true' for events that read or write to FDs, like read(), send, recvfrom(), etc."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_io_read", "Is Read", "'true' for events that read from FDs, like read(), recv(), recvfrom(), etc."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_io_write", "Is Write", "'true' for events that write to FDs, like write(), send(), etc."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "evt.io_dir", "I/O Direction", "'r' for events that read from FDs, like read(); 'w' for events that write to FDs, like write()."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_wait", "Is Wait", "'true' for events that make the thread wait, e.g. sleep(), select(), poll()."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "evt.wait_latency", "Wait Latency", "for events that make the thread wait (e.g. sleep(), select(), poll()), this is the time spent waiting for the event to return, in nanoseconds."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_syslog", "Is Syslog", "'true' for events that are writes to /dev/log."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count", "Count", "This filter field always returns 1 and can be used to count events from inside chisels."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count.error", "Error Count", "This filter field returns 1 for events that returned with an error, and can be used to count event failures from inside chisels."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count.error.file", "File Error Count", "This filter field returns 1 for events that returned with an error and are related to file I/O, and can be used to count event failures from inside chisels."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count.error.net", "Network Error Count", "This filter field returns 1 for events that returned with an error and are related to network I/O, and can be used to count event failures from inside chisels."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count.error.memory", "Memory Error Count", "This filter field returns 1 for events that returned with an error and are related to memory allocation, and can be used to count event failures from inside chisels."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count.error.other", "Other Error Count", "This filter field returns 1 for events that returned with an error and are related to none of the previous categories, and can be used to count event failures from inside chisels."},
	{PT_UINT32, EPF_NONE, PF_DEC, "evt.count.exit", "Exit Count", "This filter field returns 1 for exit events, and can be used to count single events from inside chisels."},
	{PT_UINT32, EPF_TABLE_ONLY, PF_DEC, "evt.count.procinfo", "Procinfo Count", "This filter field returns 1 for procinfo events generated by process main threads, and can be used to count processes from inside views."},
	{PT_UINT32, EPF_TABLE_ONLY, PF_DEC, "evt.count.threadinfo", "Thread Info Count", "This filter field returns 1 for procinfo events, and can be used to count processes from inside views."},
	{PT_UINT64, (filtercheck_field_flags) (EPF_FILTER_ONLY | EPF_ARG_REQUIRED), PF_DEC, "evt.around", "Around Interval", "Accepts the event if it's around the specified time interval. The syntax is evt.around[T]=D, where T is the value returned by %evt.rawtime for the event and D is a delta in milliseconds. For example, evt.around[1404996934793590564]=1000 will return the events with timestamp with one second before the timestamp and one second after it, for a total of two seconds of capture."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "evt.abspath", "Absolute Path", "Absolute path calculated from dirfd and name during syscalls like renameat and symlinkat. Use 'evt.abspath.src' or 'evt.abspath.dst' for syscalls that support multiple paths."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.in", "Input Buffer Length", "the length of the binary data buffer, but only for input I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.out", "Output Buffer Length", "the length of the binary data buffer, but only for output I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.file", "File Buffer Length", "the length of the binary data buffer, but only for file I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.file.in", "File Input Buffer Length", "the length of the binary data buffer, but only for input file I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.file.out", "File Output Buffer Length", "the length of the binary data buffer, but only for output file I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.net", "Network Buffer Length", "the length of the binary data buffer, but only for network I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.net.in", "Network Input Buffer Length", "the length of the binary data buffer, but only for input network I/O events."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "evt.buflen.net.out", "Network Output Buffer Length", "the length of the binary data buffer, but only for output network I/O events."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_open_read", "Is Opened For Reading", "'true' for open/openat/openat2/open_by_handle_at events where the path was opened for reading"},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_open_write", "Is Opened For Writing", "'true' for open/openat/openat2/open_by_handle_at events where the path was opened for writing"},
	{PT_CHARBUF, EPF_TABLE_ONLY, PF_NA, "evt.infra.docker.name", "Docker Name", "for docker infrastructure events, the name of the event."},
	{PT_CHARBUF, EPF_TABLE_ONLY, PF_NA, "evt.infra.docker.container.id", "Docker ID", "for docker infrastructure events, the id of the impacted container."},
	{PT_CHARBUF, EPF_TABLE_ONLY, PF_NA, "evt.infra.docker.container.name", "Container Name", "for docker infrastructure events, the name of the impacted container."},
	{PT_CHARBUF, EPF_TABLE_ONLY, PF_NA, "evt.infra.docker.container.image", "Container Image", "for docker infrastructure events, the image name of the impacted container."},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_open_exec", "Is Created With Execute Permissions", "'true' for open/openat/openat2/open_by_handle_at or creat events where a file is created with execute permissions"},
	{PT_BOOL, EPF_NONE, PF_NA, "evt.is_open_create", "Is Created", "'true' for for open/openat/openat2/open_by_handle_at events where a file is created."},
};

sinsp_filter_check_event::sinsp_filter_check_event()
{
	m_is_compare = false;
	m_info.m_name = "evt";
	m_info.m_shortdesc = "Syscall events only";
	m_info.m_desc = "Event fields applicable to syscall events. Note that for most events you can access the individual arguments/parameters of each syscall via evt.arg, e.g. evt.arg.filename.";
	m_info.m_fields = sinsp_filter_check_event_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_event_fields) / sizeof(sinsp_filter_check_event_fields[0]);
	m_u64val = 0;
	m_converter = new sinsp_filter_check_reference();

	m_storage_size = UESTORAGE_INITIAL_BUFSIZE;
	m_storage = (char*)malloc(m_storage_size);
	if(m_storage == NULL)
	{
		throw sinsp_exception("memory allocation error in sinsp_filter_check_appevt::sinsp_filter_check_event");
	}

	m_cargname = NULL;
}

sinsp_filter_check_event::~sinsp_filter_check_event()
{
	if(m_storage != NULL)
	{
		free(m_storage);
	}

	if(m_converter != NULL)
	{
		delete m_converter;
	}
}

sinsp_filter_check* sinsp_filter_check_event::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_event();
}

int32_t sinsp_filter_check_event::extract_arg(string fldname, string val, OUT const struct ppm_param_info** parinfo)
{
	uint32_t parsed_len = 0;

	//
	// 'arg' and 'resarg' are handled in a custom way
	//
	if(val[fldname.size()] == '[')
	{
		if(parinfo != NULL)
		{
			throw sinsp_exception("evt.arg fields must be expressed explicitly");
		}

		parsed_len = (uint32_t)val.find(']');
		string numstr = val.substr(fldname.size() + 1, parsed_len - fldname.size() - 1);

		if(m_field_id == TYPE_AROUND)
		{
			m_u64val = sinsp_numparser::parseu64(numstr);
		}
		else
		{
			m_argid = sinsp_numparser::parsed32(numstr);
		}

		parsed_len++;
	}
	else if(val[fldname.size()] == '.')
	{
		if(m_field_id == TYPE_AROUND)
		{
			throw sinsp_exception("wrong syntax for evt.around");
		}

		const struct ppm_param_info* pi =
			sinsp_utils::find_longest_matching_evt_param(val.substr(fldname.size() + 1));

		if(pi == NULL)
		{
			throw sinsp_exception("unknown event argument " + val.substr(fldname.size() + 1));
		}

		m_argname = pi->name;
		parsed_len = (uint32_t)(fldname.size() + strlen(pi->name) + 1);
		m_argid = -1;

		if(parinfo != NULL)
		{
			*parinfo = pi;
		}
	}
	else
	{
		throw sinsp_exception("filter syntax error: " + val);
	}

	return parsed_len;
}

int32_t sinsp_filter_check_event::extract_type(string fldname, string val, OUT const struct ppm_param_info** parinfo)
{
	uint32_t parsed_len = 0;

	if(val[fldname.size()] == '.')
	{
		string itype = val.substr(fldname.size() + 1);

		if(sinsp_numparser::tryparseu32(itype, &m_evtid))
		{
			m_evtid1 = PPM_EVENT_MAX;
			parsed_len = (uint32_t)(fldname.size() + itype.size() + 1);
			return parsed_len;
		}

		for(uint32_t j = 0; j < PPM_EVENT_MAX; j++)
		{
			const ppm_event_info* ei = &g_infotables.m_event_info[j];

			if(itype == ei->name)
			{
				m_evtid = j;
				m_evtid1 = j + 1;
				parsed_len = (uint32_t)(fldname.size() + strlen(ei->name) + 1);
				break;
			}
		}
	}
	else
	{
		throw sinsp_exception("filter syntax error: " + val);
	}

	return parsed_len;
}

int32_t sinsp_filter_check_event::parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering)
{
	string val(str);
	int32_t res = 0;

	//
	// A couple of fields are handled in a custom way
	//
	if(STR_MATCH("evt.arg")  && !STR_MATCH("evt.args"))
	{
		m_field_id = TYPE_ARGSTR;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("evt.arg", val, NULL);
	}
	else if(STR_MATCH("evt.rawarg"))
	{
		m_field_id = TYPE_ARGRAW;
		m_customfield = m_info.m_fields[m_field_id];
		m_field = &m_customfield;

		res = extract_arg("evt.rawarg", val, &m_arginfo);

		m_customfield.m_type = m_arginfo->type;
	}
	else if(STR_MATCH("evt.around"))
	{
		m_field_id = TYPE_AROUND;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("evt.around", val, NULL);
	}
	else if(STR_MATCH("evt.latency") ||
		STR_MATCH("evt.latency.s") ||
		STR_MATCH("evt.latency.ns") ||
		STR_MATCH("evt.latency.quantized") ||
		STR_MATCH("evt.latency.human"))
	{
		res = sinsp_filter_check::parse_field_name(str, alloc_state, needed_for_filtering);
	}
	else if(STR_MATCH("evt.abspath"))
	{
		m_field_id = TYPE_ABSPATH;
		m_field = &m_info.m_fields[m_field_id];

		if(STR_MATCH("evt.abspath.src"))
		{
			m_argid = 1;
			res = sizeof("evt.abspath.src") - 1;
		}
		else if(STR_MATCH("evt.abspath.dst"))
		{
			m_argid = 2;
			res = sizeof("evt.abspath.dst") - 1;
		}
		else
		{
			m_argid = 0;
			res = sizeof("evt.abspath") - 1;
		}
	}
	else if(STR_MATCH("evt.type.is"))
	{
		m_field_id = TYPE_TYPE_IS;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_type("evt.type.is", val, NULL);
	}
	else
	{
		res = sinsp_filter_check::parse_field_name(str, alloc_state, needed_for_filtering);
	}

	return res;
}

size_t sinsp_filter_check_event::parse_filter_value(const char* str, uint32_t len, uint8_t *storage, uint32_t storage_len)
{
	size_t parsed_len;
	if(m_field_id == sinsp_filter_check_event::TYPE_ARGRAW)
	{
		ASSERT(m_arginfo != NULL);
		parsed_len = sinsp_filter_value_parser::string_to_rawval(str, len, filter_value_p(), filter_value()->size(), m_arginfo->type);
	}
	else
	{
		parsed_len = sinsp_filter_check::parse_filter_value(str, len, storage, storage_len);
	}

	validate_filter_value(str, parsed_len);

	return parsed_len;
}



void sinsp_filter_check_event::validate_filter_value(const char* str, uint32_t len)
{
	if(m_field_id == TYPE_TYPE)
	{
		sinsp_evttables* einfo = m_inspector->get_event_info_tables();
		const struct ppm_event_info* etable = einfo->m_event_info;
		string stype(str, len);

		for(uint32_t j = 0; j < PPM_EVENT_MAX; j++)
		{
			if(stype == etable[j].name)
			{
				return;
			}
		}

		for(uint16_t j = 0; j < PPM_SC_MAX; j++)
		{
			if(stype == scap_get_ppm_sc_name((ppm_sc_code)j))
			{
				return;
			}
		}

		// note: plugins can potentially define meta-events with a certain
		// name, which will be extracted as valid values for evt.type
		// we loop over all plugins and check if at least one defines a
		// meta-event with the given name
		for (auto& p : m_inspector->get_plugin_manager()->plugins())
		{
			if (p->caps() & CAP_ASYNC)
			{
				const auto& names = p->async_event_names();
				if (names.find(stype) != names.end())
				{
					return;
				}
			}
		}

		throw sinsp_exception("unknown event type " + stype);
	}
	else if(m_field_id == TYPE_AROUND)
	{
		if(m_cmpop != CO_EQ)
		{
			throw sinsp_exception("evt.around supports only '=' comparison operator");
		}

		m_tsdelta = sinsp_numparser::parseu64(str) * 1000000;

		return;
	}
}

const filtercheck_field_info* sinsp_filter_check_event::get_field_info()
{
	if(m_field_id == TYPE_ARGRAW)
	{
		return &m_customfield;
	}
	else
	{
		return &m_info.m_fields[m_field_id];
	}
}

uint8_t* extract_argraw(sinsp_evt *evt, OUT uint32_t* len, const char *argname)
{
	const sinsp_evt_param* pi = evt->get_param_value_raw(argname);

	if(pi != NULL)
	{
		*len = pi->m_len;
		return (uint8_t*)pi->m_val;
	}
	else
	{
		return NULL;
	}
}

uint8_t *sinsp_filter_check_event::extract_abspath(sinsp_evt *evt, OUT uint32_t *len)
{
	sinsp_evt_param *parinfo;
	char *path;
	uint32_t pathlen;
	string spath;

	if(evt->m_tinfo == NULL)
	{
		return NULL;
	}

	uint16_t etype = evt->get_type();

	const char *dirfdarg = NULL, *patharg = NULL;
	if(etype == PPME_SYSCALL_RENAMEAT_X || etype == PPME_SYSCALL_RENAMEAT2_X)
	{
		if(m_argid == 0 || m_argid == 1)
		{
			dirfdarg = "olddirfd";
			patharg = "oldpath";
		}
		else if(m_argid == 2)
		{
			dirfdarg = "newdirfd";
			patharg = "newpath";
		}
	}
	else if(etype == PPME_SYSCALL_SYMLINKAT_X)
	{
		dirfdarg = "linkdirfd";
		patharg = "linkpath";
	}
	else if(etype == PPME_SYSCALL_OPENAT_E || etype == PPME_SYSCALL_OPENAT_2_X || etype == PPME_SYSCALL_OPENAT2_X)
	{
		dirfdarg = "dirfd";
		patharg = "name";
	}
	else if(etype == PPME_SYSCALL_OPEN_BY_HANDLE_AT_X)
	{
		int fd = 0;
		char fullname[SCAP_MAX_PATH_SIZE];

		//
		// We can extract the file path only in case of a successful file opening (fd>0).
		//
		parinfo = evt->get_param(0);
		ASSERT(parinfo->m_len == sizeof(int64_t));
		fd = *(int64_t *)parinfo->m_val;

		if(fd>0)
		{
			//
			// Get the file path directly from the ring buffer.
			//
			parinfo = evt->get_param(3);

			sinsp_utils::concatenate_paths(fullname, SCAP_MAX_PATH_SIZE, "", 0,
				parinfo->m_val, parinfo->m_len);

			m_strstorage = fullname;
			RETURN_EXTRACT_STRING(m_strstorage);
		}
	}
	else if(etype == PPME_SYSCALL_LINKAT_E || etype == PPME_SYSCALL_LINKAT_2_X)
	{
		if(m_argid == 0 || m_argid == 1)
		{
			dirfdarg = "olddir";
			patharg = "oldpath";
		}
		else if(m_argid == 2)
		{
			dirfdarg = "newdir";
			patharg = "newpath";
		}
	}
	else if(etype == PPME_SYSCALL_UNLINKAT_E || etype == PPME_SYSCALL_UNLINKAT_2_X)
	{
		dirfdarg = "dirfd";
		patharg = "name";
	}
	else if(etype == PPME_SYSCALL_MKDIRAT_X)
	{
		dirfdarg = "dirfd";
		patharg = "path";
	}
	else if(etype == PPME_SYSCALL_FCHMODAT_X)
	{
		dirfdarg = "dirfd";
		patharg = "filename";
	}
	else if(etype == PPME_SYSCALL_FCHOWNAT_X)
	{
		dirfdarg = "dirfd";
		patharg = "pathname";
	}

	if(!dirfdarg || !patharg)
	{
		return 0;
	}

	int dirfdargidx = -1, pathargidx = -1, idx = 0;
	while (((dirfdargidx < 0) || (pathargidx < 0)) && (idx < (int) evt->get_num_params()))
	{
		const char *name = evt->get_param_name(idx);
		if((dirfdargidx < 0) && (strcmp(name, dirfdarg) == 0))
		{
			dirfdargidx = idx;
		}
		if((pathargidx < 0) && (strcmp(name, patharg) == 0))
		{
			pathargidx = idx;
		}
		idx++;
	}

	if((dirfdargidx < 0) || (pathargidx < 0))
	{
		return 0;
	}

	parinfo = evt->get_param(dirfdargidx);
	ASSERT(parinfo->m_len == sizeof(int64_t));
	int64_t dirfd = *(int64_t *)parinfo->m_val;

	parinfo = evt->get_param(pathargidx);
	path = parinfo->m_val;
	pathlen = parinfo->m_len;

	string sdir;

	bool is_absolute = (path[0] == '/');
	if(is_absolute)
	{
		//
		// The path is absolute.
		// Some processes (e.g. irqbalance) actually do this: they pass an invalid fd and
		// and absolute path, and openat succeeds.
		//
		sdir = ".";
	}
	else if(dirfd == PPM_AT_FDCWD)
	{
		sdir = evt->m_tinfo->get_cwd();
	}
	else
	{
		evt->m_fdinfo = evt->m_tinfo->get_fd(dirfd);

		if(evt->m_fdinfo == NULL)
		{
			ASSERT(false);
			sdir = "<UNKNOWN>/";
		}
		else
		{
			if(evt->m_fdinfo->m_name[evt->m_fdinfo->m_name.length()] == '/')
			{
				sdir = evt->m_fdinfo->m_name;
			}
			else
			{
				sdir = evt->m_fdinfo->m_name + '/';
			}
		}
	}

	char fullname[SCAP_MAX_PATH_SIZE];
	sinsp_utils::concatenate_paths(fullname, SCAP_MAX_PATH_SIZE, sdir.c_str(),
		(uint32_t)sdir.length(), path, pathlen);

	m_strstorage = fullname;

	RETURN_EXTRACT_STRING(m_strstorage);
}

inline uint8_t* sinsp_filter_check_event::extract_buflen(sinsp_evt *evt, OUT uint32_t* len)
{
	if(evt->get_direction() == SCAP_ED_OUT)
	{
		sinsp_evt_param *parinfo;
		int64_t retval;

		//
		// Extract the return value
		//
		parinfo = evt->get_param(0);
		ASSERT(parinfo->m_len == sizeof(int64_t));
		retval = *(int64_t *)parinfo->m_val;

		if(retval >= 0)
		{
			RETURN_EXTRACT_PTR(parinfo->m_val);
		}
	}

	return NULL;
}

Json::Value sinsp_filter_check_event::extract_as_js(sinsp_evt *evt, OUT uint32_t* len)
{
	switch(m_field_id)
	{
	case TYPE_RUNTIME_TIME_OUTPUT_FORMAT:
		return (Json::Value::Int64)evt->get_ts();

	case TYPE_LATENCY:
	case TYPE_LATENCY_S:
	case TYPE_LATENCY_NS:
	case TYPE_DELTA:
	case TYPE_DELTA_S:
	case TYPE_DELTA_NS:
		return (Json::Value::Int64)*(uint64_t*)extract(evt, len);
	case TYPE_COUNT:
		m_u32val = 1;
		return m_u32val;

	default:
		return Json::nullValue;
	}

	return Json::nullValue;
}

uint8_t* sinsp_filter_check_event::extract_error_count(sinsp_evt *evt, OUT uint32_t* len)
{
	const sinsp_evt_param* pi = evt->get_param_value_raw("res");

	if(pi != NULL)
	{
		ASSERT(pi->m_len == sizeof(uint64_t));

		int64_t res = *(int64_t*)pi->m_val;
		if(res < 0)
		{
			m_u32val = 1;
			RETURN_EXTRACT_VAR(m_u32val);
		}
		else
		{
			return NULL;
		}
	}

	if((evt->get_info_flags() & EF_CREATES_FD) && PPME_IS_EXIT(evt->get_type()))
	{
		pi = evt->get_param_value_raw("fd");

		if(pi != NULL)
		{
			ASSERT(pi->m_len == sizeof(uint64_t));

			int64_t res = *(int64_t*)pi->m_val;
			if(res < 0)
			{
				m_u32val = 1;
				RETURN_EXTRACT_VAR(m_u32val);
			}
		}
	}

	return NULL;
}

uint8_t* sinsp_filter_check_event::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	switch(m_field_id)
	{
	case TYPE_LATENCY:
		{
			m_u64val = 0;

			if(evt->m_tinfo != NULL)
			{
				ppm_event_category ecat = evt->get_category();
				if(ecat & EC_INTERNAL)
				{
					return NULL;
				}

				m_u64val = evt->m_tinfo->m_latency;
			}

			RETURN_EXTRACT_VAR(m_u64val);
		}
	case TYPE_LATENCY_HUMAN:
		{
			m_u64val = 0;

			if(evt->m_tinfo != NULL)
			{
				ppm_event_category ecat = evt->get_category();
				if(ecat & EC_INTERNAL)
				{
					return NULL;
				}

				m_converter->set_val(PT_RELTIME,
					EPF_NONE,
					(uint8_t*)&evt->m_tinfo->m_latency,
					8,
					0,
					ppm_print_format::PF_DEC);

				m_strstorage = m_converter->tostring_nice(NULL, 0, 1000000000);
			}

			RETURN_EXTRACT_STRING(m_strstorage);
		}
	case TYPE_LATENCY_S:
	case TYPE_LATENCY_NS:
		{
			m_u64val = 0;

			if(evt->m_tinfo != NULL)
			{
				ppm_event_category ecat = evt->get_category();
				if(ecat & EC_INTERNAL)
				{
					return NULL;
				}

				uint64_t lat = evt->m_tinfo->m_latency;

				if(m_field_id == TYPE_LATENCY_S)
				{
					m_u64val = lat / 1000000000;
				}
				else
				{
					m_u64val = lat % 1000000000;
				}
			}

			RETURN_EXTRACT_VAR(m_u64val);
		}
	case TYPE_LATENCY_QUANTIZED:
		{
			if(evt->m_tinfo != NULL)
			{
				ppm_event_category ecat = evt->get_category();
				if(ecat & EC_INTERNAL)
				{
					return NULL;
				}

				uint64_t lat = evt->m_tinfo->m_latency;
				if(lat != 0)
				{
					double llatency = log10((double)lat);

					if(llatency > 11)
					{
						llatency = 11;
					}

					m_u64val = (uint64_t)(llatency * g_screen_w / 11) + 1;

					RETURN_EXTRACT_VAR(m_u64val);
				}
			}

			return NULL;
		}
	case TYPE_DELTA:
	case TYPE_DELTA_S:
	case TYPE_DELTA_NS:
		{
			if(m_u64val == 0)
			{
				m_u64val = evt->get_ts();
				m_tsdelta = 0;
			}
			else
			{
				uint64_t tts = evt->get_ts();

				if(m_field_id == TYPE_DELTA)
				{
					m_tsdelta = tts - m_u64val;
				}
				else if(m_field_id == TYPE_DELTA_S)
				{
					m_tsdelta = (tts - m_u64val) / ONE_SECOND_IN_NS;
				}
				else if(m_field_id == TYPE_DELTA_NS)
				{
					m_tsdelta = (tts - m_u64val) % ONE_SECOND_IN_NS;
				}

				m_u64val = tts;
			}

			RETURN_EXTRACT_VAR(m_tsdelta);
		}
	case TYPE_RUNTIME_TIME_OUTPUT_FORMAT:
		{
			char timebuffer[100];
			m_strstorage = "";
			switch(m_inspector->m_output_time_flag)
			{
				case 'h':
					sinsp_utils::ts_to_string(evt->get_ts(), &m_strstorage, false, true);
					RETURN_EXTRACT_STRING(m_strstorage);

				case 'a':
					m_strstorage += to_string(evt->get_ts() / ONE_SECOND_IN_NS);
					m_strstorage += ".";
					m_strstorage += to_string(evt->get_ts() % ONE_SECOND_IN_NS);
					RETURN_EXTRACT_STRING(m_strstorage);

				case 'r':
					m_strstorage += to_string((evt->get_ts() - m_inspector->m_firstevent_ts) / ONE_SECOND_IN_NS);
					m_strstorage += ".";
					snprintf(timebuffer, sizeof(timebuffer), "%09llu", (evt->get_ts() - m_inspector->m_firstevent_ts) % ONE_SECOND_IN_NS);
					m_strstorage += string(timebuffer);
					RETURN_EXTRACT_STRING(m_strstorage);

				case 'd':
				{
					if(evt->m_tinfo != NULL)
					{
						long long unsigned lat = evt->m_tinfo->m_latency;

						m_strstorage += to_string(lat / 1000000000);
						m_strstorage += ".";
						snprintf(timebuffer, sizeof(timebuffer), "%09llu", lat % 1000000000);
						m_strstorage += string(timebuffer);
					}
					else
					{
						m_strstorage = "0.000000000";
					}

					RETURN_EXTRACT_STRING(m_strstorage);
				}

				case 'D':
					if(m_u64val == 0)
					{
						m_u64val = evt->get_ts();
						m_tsdelta = 0;
					}
					uint64_t tts = evt->get_ts();

					m_strstorage += to_string((tts - m_u64val) / ONE_SECOND_IN_NS);
					m_tsdelta = (tts - m_u64val) / ONE_SECOND_IN_NS;
					m_strstorage += ".";
					snprintf(timebuffer, sizeof(timebuffer), "%09llu", (tts - m_u64val) % ONE_SECOND_IN_NS);
					m_strstorage += string(timebuffer);
					m_tsdelta = (tts - m_u64val) % ONE_SECOND_IN_NS;

					m_u64val = tts;
					RETURN_EXTRACT_STRING(m_strstorage);
			}
		}
	case TYPE_DIR:
		if(PPME_IS_ENTER(evt->get_type()))
		{
			RETURN_EXTRACT_CSTR(">");
		}
		else
		{
			RETURN_EXTRACT_CSTR("<");
		}
	case TYPE_TYPE:
		{
			uint8_t* evname;
			uint16_t etype = evt->m_pevt->type;

			if(etype == PPME_GENERIC_E || etype == PPME_GENERIC_X)
			{
				sinsp_evt_param *parinfo = evt->get_param(0);
				ASSERT(parinfo->m_len == sizeof(uint16_t));
				uint16_t ppm_sc = *(uint16_t *)parinfo->m_val;

				// Only generic enter event has the nativeID as second param
				if(m_inspector->is_capture() && ppm_sc == PPM_SC_UNKNOWN && etype == PPME_GENERIC_E)
				{
					// try to enforce a forward compatibility for syscalls added
					// after a scap file was generated,
					// by looking up using nativeID.
					// Of course, this will only reliably work for
					// same architecture scap capture->replay.
					parinfo = evt->get_param(1);
					ASSERT(parinfo->m_len == sizeof(uint16_t));
					uint16_t nativeid = *(uint16_t *)parinfo->m_val;
					ppm_sc = scap_native_id_to_ppm_sc(nativeid);
				}
				evname = (uint8_t*)scap_get_ppm_sc_name((ppm_sc_code)ppm_sc);
			}
			else
			{
				// note: for async events, the event name is encoded
				// inside the event itself. In this case libsinsp's evt.type
				// field acts as an alias of evt.asynctype.
				if (etype == PPME_ASYNCEVENT_E)
				{
					evname = (uint8_t*) evt->get_param(1)->m_val;
				}
				else
				{
					evname = (uint8_t*)evt->get_name();
				}
			}

			RETURN_EXTRACT_CSTR(evname);
		}
		break;
	case TYPE_TYPE_IS:
		{
			uint16_t etype = evt->m_pevt->type;

			if(etype == m_evtid || etype == m_evtid1)
			{
				m_u32val = 1;
			}
			else
			{
				m_u32val = 0;
			}

			RETURN_EXTRACT_VAR(m_u32val);
		}
		break;
	case TYPE_SYSCALL_TYPE:
		{
			uint8_t* evname;
			ppm_event_code etype = (ppm_event_code)evt->m_pevt->type;
			if(!libsinsp::events::is_syscall_event(etype))
			{
				return NULL;
			}

			if(etype == PPME_GENERIC_E || etype == PPME_GENERIC_X)
			{
				sinsp_evt_param *parinfo = evt->get_param(0);
				ASSERT(parinfo->m_len == sizeof(uint16_t));
				uint16_t ppm_sc = *(uint16_t *)parinfo->m_val;

				// Only generic enter event has the nativeID as second param
				if (m_inspector->is_capture() && ppm_sc == PPM_SC_UNKNOWN && etype == PPME_GENERIC_E)
				{
					// try to enforce a forward compatibility for syscalls added
					// after a scap file was generated,
					// by looking up using nativeID.
					// Of course, this will only reliably work for
					// same architecture scap capture->replay.
					parinfo = evt->get_param(1);
					ASSERT(parinfo->m_len == sizeof(uint16_t));
					uint16_t nativeid = *(uint16_t *)parinfo->m_val;
					ppm_sc = scap_native_id_to_ppm_sc(nativeid);
				}
				evname = (uint8_t*)scap_get_ppm_sc_name((ppm_sc_code)ppm_sc);
			}
			else
			{
				evname = (uint8_t*)evt->get_name();
			}

			RETURN_EXTRACT_CSTR(evname);
		}
		break;
	case TYPE_CATEGORY:
		sinsp_evt::category cat;
		evt->get_category(&cat);

		switch(cat.m_category)
		{
		case EC_UNKNOWN:
			m_strstorage = "unknown";
			break;
		case EC_OTHER:
			m_strstorage = "other";
			break;
		case EC_FILE:
			m_strstorage = "file";
			break;
		case EC_NET:
			m_strstorage = "net";
			break;
		case EC_IPC:
			m_strstorage = "IPC";
			break;
		case EC_MEMORY:
			m_strstorage = "memory";
			break;
		case EC_PROCESS:
			m_strstorage = "process";
			break;
		case EC_SLEEP:
			m_strstorage = "sleep";
			break;
		case EC_SYSTEM:
			m_strstorage = "system";
			break;
		case EC_SIGNAL:
			m_strstorage = "signal";
			break;
		case EC_USER:
			m_strstorage = "user";
			break;
		case EC_TIME:
			m_strstorage = "time";
			break;
		case EC_PROCESSING:
			m_strstorage = "processing";
			break;
		case EC_IO_READ:
		case EC_IO_WRITE:
		case EC_IO_OTHER:
		{
			switch(cat.m_subcategory)
			{
			case sinsp_evt::SC_FILE:
				m_strstorage = "file";
				break;
			case sinsp_evt::SC_NET:
				m_strstorage = "net";
				break;
			case sinsp_evt::SC_IPC:
				m_strstorage = "ipc";
				break;
			case sinsp_evt::SC_NONE:
			case sinsp_evt::SC_UNKNOWN:
			case sinsp_evt::SC_OTHER:
				m_strstorage = "unknown";
				break;
			default:
				ASSERT(false);
				m_strstorage = "unknown";
				break;
			}
		}
		break;
		case EC_WAIT:
			m_strstorage = "wait";
			break;
		case EC_SCHEDULER:
			m_strstorage = "scheduler";
			break;
		case EC_INTERNAL:
			m_strstorage = "internal";
			break;
		case EC_SYSCALL:
			m_strstorage = "syscall";
			break;
		case EC_TRACEPOINT:
			m_strstorage = "tracepoint";
			break;
		case EC_PLUGIN:
			m_strstorage = "plugin";
			break;
		case EC_METAEVENT:
			m_strstorage = "meta";
			break;
		default:
			m_strstorage = "unknown";
			break;
		}

		RETURN_EXTRACT_STRING(m_strstorage);
	case TYPE_CPU:
		RETURN_EXTRACT_VAR(evt->m_cpuid);
	case TYPE_ARGRAW:
		return extract_argraw(evt, len, m_arginfo->name);
		break;
	case TYPE_ARGSTR:
		{
			const char* resolved_argstr;
			const char* argstr;

			ASSERT(m_inspector != NULL);

			if(m_argid != -1)
			{
				if(m_argid >= (int32_t)evt->get_num_params())
				{
					return NULL;
				}

				argstr = evt->get_param_as_str(m_argid, &resolved_argstr, m_inspector->get_buffer_format());
			}
			else
			{
				argstr = evt->get_param_value_str(m_argname.c_str(), &resolved_argstr, m_inspector->get_buffer_format());
			}

			if(resolved_argstr != NULL && resolved_argstr[0] != 0)
			{
				RETURN_EXTRACT_CSTR(resolved_argstr);
			}
			else
			{
				RETURN_EXTRACT_CSTR(argstr);
			}
		}
		break;
	case TYPE_INFO:
		{
			sinsp_fdinfo_t* fdinfo = evt->m_fdinfo;

			if(fdinfo != NULL && fdinfo->m_callbacks != NULL)
			{
				char* il;
				vector<sinsp_protodecoder*>* cbacks = &(fdinfo->m_callbacks->m_write_callbacks);

				for(auto it = cbacks->begin(); it != cbacks->end(); ++it)
				{
					if((*it)->get_info_line(&il))
					{
						RETURN_EXTRACT_CSTR(il);
					}
				}
			}
		}
		//
		// NOTE: this falls through to TYPE_ARGSTR, and that's what we want!
		//       Please don't add anything here!
		//
	case TYPE_ARGS:
		{
			if(evt->get_type() == PPME_GENERIC_E || evt->get_type() == PPME_GENERIC_X)
			{
				//
				// Don't print the arguments for generic events: they have only internal use
				//
				RETURN_EXTRACT_CSTR("");
			}

			const char* resolved_argstr = NULL;
			const char* argstr = NULL;
			uint32_t nargs = evt->get_num_params();
			m_strstorage.clear();

			for(uint32_t j = 0; j < nargs; j++)
			{
				ASSERT(m_inspector != NULL);

				argstr = evt->get_param_as_str(j, &resolved_argstr, m_inspector->get_buffer_format());

				if(resolved_argstr[0] == 0)
				{
					m_strstorage += evt->get_param_name(j);
					m_strstorage += '=';
					m_strstorage += argstr;
					m_strstorage += " ";
				}
				else
				{
					m_strstorage += evt->get_param_name(j);
					m_strstorage += '=';
					m_strstorage += argstr;
					m_strstorage += string("(") + resolved_argstr + ") ";
				}
			}

			RETURN_EXTRACT_STRING(m_strstorage);
		}
		break;
	case TYPE_BUFFER:
		{
			if(m_is_compare)
			{
				return extract_argraw(evt, len, "data");
			}

			const char* resolved_argstr;
			const char* argstr;
			argstr = evt->get_param_value_str("data", &resolved_argstr, m_inspector->get_buffer_format());
			*len = evt->m_rawbuf_str_len;

			return (uint8_t*)argstr;
		}
	case TYPE_BUFLEN:
		if(evt->m_fdinfo && evt->get_category() & EC_IO_BASE)
		{
			return extract_buflen(evt, len);
		}
		break;
	case TYPE_RESRAW:
		{
			const sinsp_evt_param* pi = evt->get_param_value_raw("res");

			if(pi != NULL)
			{
				*len = pi->m_len;
				return (uint8_t*)pi->m_val;
			}

			if((evt->get_info_flags() & EF_CREATES_FD) && PPME_IS_EXIT(evt->get_type()))
			{
				pi = evt->get_param_value_raw("fd");

				if(pi != NULL)
				{
					*len = pi->m_len;
					return (uint8_t*)pi->m_val;
				}
			}

			return NULL;
		}
		break;
	case TYPE_RESSTR:
		{
			const char* resolved_argstr;
			const char* argstr;

			const sinsp_evt_param* pi = evt->get_param_value_raw("res");

			if(pi != NULL)
			{
				ASSERT(pi->m_len == sizeof(int64_t));

				int64_t res = *(int64_t*)pi->m_val;

				if(res >= 0)
				{
					RETURN_EXTRACT_CSTR("SUCCESS");
				}
				else
				{
					argstr = evt->get_param_value_str("res", &resolved_argstr);
					ASSERT(resolved_argstr != NULL && resolved_argstr[0] != 0);

					if(resolved_argstr != NULL && resolved_argstr[0] != 0)
					{
						RETURN_EXTRACT_CSTR(resolved_argstr);
					}
					else if(argstr != NULL)
					{
						RETURN_EXTRACT_CSTR(argstr);
					}
				}
			}
			else
			{
				if((evt->get_info_flags() & EF_CREATES_FD) && PPME_IS_EXIT(evt->get_type()))
				{
					pi = evt->get_param_value_raw("fd");
					if (pi)
					{
						int64_t res = *(int64_t*)pi->m_val;

						if(res >= 0)
						{
							RETURN_EXTRACT_CSTR("SUCCESS");
						}
						else
						{
							argstr = evt->get_param_value_str("fd", &resolved_argstr);
							ASSERT(resolved_argstr != NULL && resolved_argstr[0] != 0);

							if(resolved_argstr != NULL && resolved_argstr[0] != 0)
							{
								RETURN_EXTRACT_CSTR(resolved_argstr);
							}
							else if(argstr != NULL)
							{
								RETURN_EXTRACT_CSTR(argstr);
							}
						}
					}
				}
			}

			return NULL;
		}
		break;
	case TYPE_FAILED:
		{
			m_u32val = 0;
			const sinsp_evt_param* pi = evt->get_param_value_raw("res");

			if(pi != NULL)
			{
				ASSERT(pi->m_len == sizeof(int64_t));
				if(*(int64_t*)pi->m_val < 0)
				{
					m_u32val = 1;
				}
			}
			else if((evt->get_info_flags() & EF_CREATES_FD) && PPME_IS_EXIT(evt->get_type()))
			{
				pi = evt->get_param_value_raw("fd");

				if(pi != NULL)
				{
					ASSERT(pi->m_len == sizeof(int64_t));
					if(*(int64_t*)pi->m_val < 0)
					{
						m_u32val = 1;
					}
				}
			}

			RETURN_EXTRACT_VAR(m_u32val);
		}
		break;
	case TYPE_ISIO:
		{
			ppm_event_flags eflags = evt->get_info_flags();
			if(eflags & (EF_READS_FROM_FD | EF_WRITES_TO_FD))
			{
				m_u32val = 1;
			}
			else
			{
				m_u32val = 0;
			}
		}

		RETURN_EXTRACT_VAR(m_u32val);
	case TYPE_ISIO_READ:
		{
			ppm_event_flags eflags = evt->get_info_flags();
			if(eflags & EF_READS_FROM_FD)
			{
				m_u32val = 1;
			}
			else
			{
				m_u32val = 0;
			}

			RETURN_EXTRACT_VAR(m_u32val);
		}
	case TYPE_ISIO_WRITE:
		{
			ppm_event_flags eflags = evt->get_info_flags();
			if(eflags & EF_WRITES_TO_FD)
			{
				m_u32val = 1;
			}
			else
			{
				m_u32val = 0;
			}

			RETURN_EXTRACT_VAR(m_u32val);
		}
	case TYPE_IODIR:
		{
			ppm_event_flags eflags = evt->get_info_flags();
			if(eflags & EF_WRITES_TO_FD)
			{
				m_strstorage = "write";
			}
			else if(eflags & EF_READS_FROM_FD)
			{
				m_strstorage = "read";
			}
			else
			{
				return NULL;
			}

			RETURN_EXTRACT_STRING(m_strstorage);
		}
	case TYPE_ISWAIT:
		{
			ppm_event_flags eflags = evt->get_info_flags();
			if(eflags & (EF_WAITS))
			{
				m_u32val = 1;
			}
			else
			{
				m_u32val = 0;
			}
		}

		RETURN_EXTRACT_VAR(m_u32val);
	case TYPE_WAIT_LATENCY:
		{
			ppm_event_flags eflags = evt->get_info_flags();
			uint16_t etype = evt->m_pevt->type;

			if(eflags & (EF_WAITS) && PPME_IS_EXIT(etype))
			{
				if(evt->m_tinfo != NULL)
				{
					m_u64val = evt->m_tinfo->m_latency;
				}
				else
				{
					m_u64val = 0;
				}

				RETURN_EXTRACT_VAR(m_u64val);
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_ISSYSLOG:
		{
			m_u32val = 0;

			ppm_event_flags eflags = evt->get_info_flags();
			if(eflags & EF_WRITES_TO_FD)
			{
				sinsp_fdinfo_t* fdinfo = evt->m_fdinfo;

				if(fdinfo != NULL)
				{
					if(fdinfo->m_name.find("/dev/log") != string::npos)
					{
						m_u32val = 1;
					}
				}
			}

			RETURN_EXTRACT_VAR(m_u32val);
		}
	case TYPE_COUNT:
		m_u32val = 1;
		RETURN_EXTRACT_VAR(m_u32val);
	case TYPE_COUNT_ERROR:
		return extract_error_count(evt, len);
	case TYPE_COUNT_ERROR_FILE:
		{
			sinsp_fdinfo_t* fdinfo = evt->m_fdinfo;

			if(fdinfo != NULL)
			{
				if(fdinfo->m_type == SCAP_FD_FILE ||
					fdinfo->m_type == SCAP_FD_FILE_V2 ||
					fdinfo->m_type == SCAP_FD_DIRECTORY)
				{
					return extract_error_count(evt, len);
				}
			}
			else
			{
				uint16_t etype = evt->get_type();

				if(etype == PPME_SYSCALL_OPEN_X ||
					etype == PPME_SYSCALL_CREAT_X ||
					etype == PPME_SYSCALL_OPENAT_X ||
					etype == PPME_SYSCALL_OPENAT_2_X ||
					etype == PPME_SYSCALL_OPENAT2_X ||
					etype == PPME_SYSCALL_OPEN_BY_HANDLE_AT_X)
				{
					return extract_error_count(evt, len);
				}
			}

			return NULL;
		}
	case TYPE_COUNT_ERROR_NET:
		{
			sinsp_fdinfo_t* fdinfo = evt->m_fdinfo;

			if(fdinfo != NULL)
			{
				if(fdinfo->m_type == SCAP_FD_IPV4_SOCK ||
					fdinfo->m_type == SCAP_FD_IPV6_SOCK ||
					fdinfo->m_type == SCAP_FD_IPV4_SERVSOCK ||
					fdinfo->m_type == SCAP_FD_IPV6_SERVSOCK ||
					fdinfo->m_type == SCAP_FD_UNIX_SOCK)
				{
					return extract_error_count(evt, len);
				}
			}
			else
			{
				uint16_t etype = evt->get_type();

				if(etype == PPME_SOCKET_ACCEPT_X ||
					etype == PPME_SOCKET_ACCEPT_5_X ||
					etype == PPME_SOCKET_ACCEPT4_X ||
					etype == PPME_SOCKET_ACCEPT4_5_X ||
				        etype == PPME_SOCKET_ACCEPT4_6_X ||
					etype == PPME_SOCKET_CONNECT_X)
				{
					return extract_error_count(evt, len);
				}
			}

			return NULL;
		}
	case TYPE_COUNT_ERROR_MEMORY:
		{
			if(evt->get_category() == EC_MEMORY)
			{
				return extract_error_count(evt, len);
			}
			else
			{
				return NULL;
			}
		}
	case TYPE_COUNT_ERROR_OTHER:
		{
			sinsp_fdinfo_t* fdinfo = evt->m_fdinfo;

			if(fdinfo != NULL)
			{
				if(!(fdinfo->m_type == SCAP_FD_FILE ||
					fdinfo->m_type == SCAP_FD_FILE_V2 ||
					fdinfo->m_type == SCAP_FD_DIRECTORY ||
					fdinfo->m_type == SCAP_FD_IPV4_SOCK ||
					fdinfo->m_type == SCAP_FD_IPV6_SOCK ||
					fdinfo->m_type == SCAP_FD_IPV4_SERVSOCK ||
					fdinfo->m_type == SCAP_FD_IPV6_SERVSOCK ||
					fdinfo->m_type == SCAP_FD_UNIX_SOCK))
				{
					return extract_error_count(evt, len);
				}
			}
			else
			{
				uint16_t etype = evt->get_type();

				if(!(etype == PPME_SYSCALL_OPEN_X ||
					etype == PPME_SYSCALL_CREAT_X ||
					etype == PPME_SYSCALL_OPENAT_X ||
					etype == PPME_SYSCALL_OPENAT_2_X ||
					etype == PPME_SYSCALL_OPENAT2_X ||
					etype == PPME_SYSCALL_OPEN_BY_HANDLE_AT_X ||
					etype == PPME_SOCKET_ACCEPT_X ||
					etype == PPME_SOCKET_ACCEPT_5_X ||
					etype == PPME_SOCKET_ACCEPT4_X ||
					etype == PPME_SOCKET_ACCEPT4_5_X ||
				        etype == PPME_SOCKET_ACCEPT4_6_X ||
					etype == PPME_SOCKET_CONNECT_X ||
					evt->get_category() == EC_MEMORY))
				{
					return extract_error_count(evt, len);
				}
			}

			return NULL;
		}
	case TYPE_COUNT_EXIT:
		if(PPME_IS_EXIT(evt->get_type()))
		{
			m_u32val = 1;
			RETURN_EXTRACT_VAR(m_u32val);
		}
		else
		{
			return NULL;
		}
	case TYPE_COUNT_PROCINFO:
		{
			uint16_t etype = evt->get_type();

			if(etype == PPME_PROCINFO_E)
			{
				sinsp_threadinfo* tinfo = evt->get_thread_info();

				if(tinfo != NULL && tinfo->is_main_thread())
				{
					m_u32val = 1;
					RETURN_EXTRACT_VAR(m_u32val);
				}
			}
		}

		break;
	case TYPE_COUNT_THREADINFO:
		{
			uint16_t etype = evt->get_type();

			if(etype == PPME_PROCINFO_E)
			{
				m_u32val = 1;
				RETURN_EXTRACT_VAR(m_u32val);
			}
		}

		break;
	case TYPE_ABSPATH:
		return extract_abspath(evt, len);
	case TYPE_BUFLEN_IN:
		if(evt->m_fdinfo && evt->get_category() == EC_IO_READ)
		{
			return extract_buflen(evt, len);
		}

		break;
	case TYPE_BUFLEN_OUT:
		if(evt->m_fdinfo && evt->get_category() == EC_IO_WRITE)
		{
			return extract_buflen(evt, len);
		}

		break;
	case TYPE_BUFLEN_FILE:
		if(evt->m_fdinfo && evt->get_category() & EC_IO_BASE)
		{
			if(evt->m_fdinfo->m_type == SCAP_FD_FILE || evt->m_fdinfo->m_type == SCAP_FD_FILE_V2)
			{
				return extract_buflen(evt, len);
			}
		}

		break;
	case TYPE_BUFLEN_FILE_IN:
		if(evt->m_fdinfo && evt->get_category() == EC_IO_READ)
		{
			if(evt->m_fdinfo->m_type == SCAP_FD_FILE || evt->m_fdinfo->m_type == SCAP_FD_FILE_V2)
			{
				return extract_buflen(evt, len);
			}
		}

		break;
	case TYPE_BUFLEN_FILE_OUT:
		if(evt->m_fdinfo && evt->get_category() == EC_IO_WRITE)
		{
			if(evt->m_fdinfo->m_type == SCAP_FD_FILE || evt->m_fdinfo->m_type == SCAP_FD_FILE_V2)
			{
				return extract_buflen(evt, len);
			}
		}

		break;
	case TYPE_BUFLEN_NET:
		if(evt->m_fdinfo && evt->get_category() & EC_IO_BASE)
		{
			scap_fd_type etype = evt->m_fdinfo->m_type;

			if(etype >= SCAP_FD_IPV4_SOCK && etype <= SCAP_FD_IPV6_SERVSOCK)
			{
				return extract_buflen(evt, len);
			}
		}

		break;
	case TYPE_BUFLEN_NET_IN:
		if(evt->m_fdinfo && evt->get_category() == EC_IO_READ)
		{
			scap_fd_type etype = evt->m_fdinfo->m_type;

			if(etype >= SCAP_FD_IPV4_SOCK && etype <= SCAP_FD_IPV6_SERVSOCK)
			{
				return extract_buflen(evt, len);
			}
		}

		break;
	case TYPE_BUFLEN_NET_OUT:
		if(evt->m_fdinfo && evt->get_category() == EC_IO_WRITE)
		{
			scap_fd_type etype = evt->m_fdinfo->m_type;

			if(etype >= SCAP_FD_IPV4_SOCK && etype <= SCAP_FD_IPV6_SERVSOCK)
			{
				return extract_buflen(evt, len);
			}
		}

		break;
	case TYPE_ISOPEN_READ:
	case TYPE_ISOPEN_WRITE:
	case TYPE_ISOPEN_EXEC:
	case TYPE_ISOPEN_CREATE:
		{
			uint16_t etype = evt->get_type();

			m_u32val = 0;
			sinsp_evt_param *parinfo;
			// If any of the exec bits is on, we consider this an open+exec
			uint32_t is_exec_mask = (PPM_S_IXUSR | PPM_S_IXGRP | PPM_S_IXOTH);

			if(etype == PPME_SYSCALL_OPEN_X ||
			   etype == PPME_SYSCALL_OPENAT_E ||
			   etype == PPME_SYSCALL_OPENAT_2_X ||
			   etype == PPME_SYSCALL_OPENAT2_X ||
			   etype == PPME_SYSCALL_OPEN_BY_HANDLE_AT_X)
			{
				bool is_new_version = etype == PPME_SYSCALL_OPENAT_2_X || etype == PPME_SYSCALL_OPENAT2_X;
				// For both OPEN_X and OPENAT_E,
				// flags is the 3rd argument.
				parinfo = evt->get_param(is_new_version ? 3 : 2);
				ASSERT(parinfo->m_len == sizeof(uint32_t));
				uint32_t flags = *(uint32_t *)parinfo->m_val;

				// PPM open flags use 0x11 for
				// PPM_O_RDWR, so there's no need to
				// check that value explicitly.
				if(m_field_id == TYPE_ISOPEN_READ &&
				   flags & PPM_O_RDONLY)
				{
					m_u32val = 1;
				}

				if(m_field_id == TYPE_ISOPEN_WRITE &&
				   flags & PPM_O_WRONLY)
				{
					m_u32val = 1;
				}

				if(m_field_id == TYPE_ISOPEN_CREATE)
				{
					// If PPM_O_F_CREATED is set the file is created 
					if(flags & PPM_O_F_CREATED)
					{
						m_u32val = 1;
					}

					// If PPM_O_TMPFILE is set and syscall is successful the file is created
					if(flags & PPM_O_TMPFILE)
					{
						parinfo = evt->get_param(0);
						ASSERT(parinfo->m_len == sizeof(int64_t));
						int64_t retval = *(int64_t *)parinfo->m_val;

						if(retval >= 0)
						{
							m_u32val = 1;
						}
					}
				}

				/* `open_by_handle_at` exit event has no `mode` parameter. */
				if(m_field_id == TYPE_ISOPEN_EXEC && (flags & (PPM_O_TMPFILE | PPM_O_CREAT) && etype != PPME_SYSCALL_OPEN_BY_HANDLE_AT_X))
				{
					parinfo = evt->get_param(is_new_version ? 4 : 3);
					ASSERT(parinfo->m_len == sizeof(uint32_t));
					uint32_t mode_bits = *(uint32_t *)parinfo->m_val;
					m_u32val = (mode_bits & is_exec_mask)? 1 : 0;
				}
			}
			else if ((m_field_id == TYPE_ISOPEN_EXEC) && (etype == PPME_SYSCALL_CREAT_X))
			{
				parinfo = evt->get_param(2);
				ASSERT(parinfo->m_len == sizeof(uint32_t));
				uint32_t mode_bits = *(uint32_t *)parinfo->m_val;
				m_u32val = (mode_bits & is_exec_mask)? 1 : 0;
			}

			RETURN_EXTRACT_VAR(m_u32val);
		}
		
		break;
	case TYPE_INFRA_DOCKER_NAME:
	case TYPE_INFRA_DOCKER_CONTAINER_ID:
	case TYPE_INFRA_DOCKER_CONTAINER_NAME:
	case TYPE_INFRA_DOCKER_CONTAINER_IMAGE:
		{
			uint16_t etype = evt->m_pevt->type;

			if(etype == PPME_INFRASTRUCTURE_EVENT_E)
			{
				sinsp_evt_param* parinfo = evt->get_param(2);
				char* descstr = (char*)parinfo->m_val;
				vector<string> elements = sinsp_split(descstr, ';');
				for(string ute : elements)
				{
					string e = trim(ute);

					if(m_field_id == TYPE_INFRA_DOCKER_NAME)
					{
						if(e.substr(0, sizeof("Event") - 1) == "Event")
						{
							vector<string> subelements = sinsp_split(e, ':');
							ASSERT(subelements.size() == 2);
							m_strstorage = trim(subelements[1]);
							RETURN_EXTRACT_STRING(m_strstorage);
						}
					}
					else if(m_field_id == TYPE_INFRA_DOCKER_CONTAINER_ID)
					{
						if(e.substr(0, sizeof("ID") - 1) == "ID")
						{
							vector<string> subelements = sinsp_split(e, ':');
							ASSERT(subelements.size() == 2);
							m_strstorage = trim(subelements[1]);
							if(m_strstorage.length() > 12)
							{
								m_strstorage = m_strstorage.substr(0, 12);
							}
							RETURN_EXTRACT_STRING(m_strstorage);
						}
					}
					else if(m_field_id == TYPE_INFRA_DOCKER_CONTAINER_NAME)
					{
						if(e.substr(0, sizeof("name") - 1) == "name")
						{
							vector<string> subelements = sinsp_split(e, ':');
							ASSERT(subelements.size() == 2);
							m_strstorage = trim(subelements[1]);
							RETURN_EXTRACT_STRING(m_strstorage);
						}
					}
					else if(m_field_id == TYPE_INFRA_DOCKER_CONTAINER_IMAGE)
					{
						if(e.substr(0, sizeof("Image") - 1) == "Image")
						{
							vector<string> subelements = sinsp_split(e, ':');
							ASSERT(subelements.size() == 2);
							m_strstorage = subelements[1];

							if(m_strstorage.find("@") != string::npos)
							{
								m_strstorage = m_strstorage.substr(0, m_strstorage.find("@"));
							}
							else if(m_strstorage.find("sha256") != string::npos)
							{
								m_strstorage = e.substr(e.find(":") + 1);
							}
							m_strstorage = trim(m_strstorage);
							RETURN_EXTRACT_STRING(m_strstorage);
						}
					}
				}
			}
		}
		break;
	default:
		ASSERT(false);
		return NULL;
	}

	return NULL;
}

bool sinsp_filter_check_event::compare(sinsp_evt *evt)
{
	bool res;

	m_is_compare = true;

	if(m_field_id == TYPE_ARGRAW)
	{
		uint32_t len;
		bool sanitize_strings = false;
		// note: this uses the single-value extract because this filtercheck
		// class does not support multi-valued extraction
		uint8_t* extracted_val = extract(evt, &len, sanitize_strings);

		if(extracted_val == NULL)
		{
			return false;
		}

		ASSERT(m_arginfo != NULL);

		res = flt_compare(m_cmpop,
			m_arginfo->type,
			extracted_val);
	}
	else if(m_field_id == TYPE_AROUND)
	{
		uint64_t ts = evt->get_ts();
		uint64_t t1 = ts - m_tsdelta;
		uint64_t t2 = ts + m_tsdelta;

		bool res1 = ::flt_compare(CO_GE,
			PT_UINT64,
			&m_u64val,
			&t1);

		bool res2 = ::flt_compare(CO_LE,
			PT_UINT64,
			&m_u64val,
			&t2);

		return res1 && res2;
	}
	else
	{
		res = sinsp_filter_check::compare(evt);
	}

	m_is_compare = false;

	return res;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_user implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_user_fields[] =
{
	{PT_UINT32, EPF_NONE, PF_ID, "user.uid", "User ID", "user ID."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "user.name", "User Name", "user name."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "user.homedir", "Home Directory", "home directory of the user."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "user.shell", "Shell", "user's shell."},
	{PT_INT64, EPF_NONE, PF_ID, "user.loginuid", "Login User ID", "audit user id (auid), internally the loginuid is of type `uint32_t`. However, if an invalid uid corresponding to UINT32_MAX is encountered, it is returned as -1 to support familiar filtering conditions."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "user.loginname", "Login User Name", "audit user name (auid)."},
};

sinsp_filter_check_user::sinsp_filter_check_user()
{
	m_info.m_name = "user";
	m_info.m_desc = "Information about the user executing the specific event.";
	m_info.m_fields = sinsp_filter_check_user_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_user_fields) / sizeof(sinsp_filter_check_user_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;
}

sinsp_filter_check* sinsp_filter_check_user::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_user();
}

uint8_t* sinsp_filter_check_user::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	sinsp_threadinfo* tinfo = evt->get_thread_info();

	if(tinfo == NULL)
	{
		return NULL;
	}

	// For container events, use the user from the container metadata instead.
	if(m_field_id == TYPE_NAME &&
	   (evt->get_type() == PPME_CONTAINER_JSON_E || evt->get_type() == PPME_CONTAINER_JSON_2_E))
	{
		const sinsp_container_info::ptr_t container_info =
			m_inspector->m_container_manager.get_container(tinfo->m_container_id);

		if(!container_info)
		{
			return NULL;
		}

		RETURN_EXTRACT_STRING(container_info->m_container_user);
	}

	switch(m_field_id)
	{
	case TYPE_UID:
		RETURN_EXTRACT_VAR(tinfo->m_user.uid);
	case TYPE_NAME:
		RETURN_EXTRACT_CSTR(tinfo->m_user.name);
	case TYPE_HOMEDIR:
		RETURN_EXTRACT_CSTR(tinfo->m_user.homedir);
	case TYPE_SHELL:
		RETURN_EXTRACT_CSTR(tinfo->m_user.shell);
	case TYPE_LOGINUID:
		m_s64val = (int64_t)-1;
		if(tinfo->m_loginuser.uid < UINT32_MAX)
		{
			m_s64val = (int64_t)tinfo->m_loginuser.uid;
		}
		RETURN_EXTRACT_VAR(m_s64val);
	case TYPE_LOGINNAME:
		RETURN_EXTRACT_CSTR(tinfo->m_loginuser.name);
	default:
		ASSERT(false);
		break;
	}

	return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_group implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_group_fields[] =
{
	{PT_UINT32, EPF_NONE, PF_ID, "group.gid", "Group ID", "group ID."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "group.name", "Group Name", "group name."},
};

sinsp_filter_check_group::sinsp_filter_check_group()
{
	m_info.m_name = "group";
	m_info.m_desc = "Information about the user group.";
	m_info.m_fields = sinsp_filter_check_group_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_group_fields) / sizeof(sinsp_filter_check_group_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;
}

sinsp_filter_check* sinsp_filter_check_group::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_group();
}

uint8_t* sinsp_filter_check_group::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	sinsp_threadinfo* tinfo = evt->get_thread_info();

	if(tinfo == NULL)
	{
		return NULL;
	}

	switch(m_field_id)
	{
	case TYPE_GID:
		RETURN_EXTRACT_VAR(tinfo->m_group.gid);
	case TYPE_NAME:
		RETURN_EXTRACT_CSTR(tinfo->m_group.name);
	default:
		ASSERT(false);
		break;
	}

	return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_tracer implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_tracer_fields[] =
{
	{PT_INT64, EPF_NONE, PF_ID, "span.id", "Span ID", "ID of the span. This is a unique identifier that is used to match the enter and exit tracer events for this span. It can also be used to match different spans belonging to a trace."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "span.time", "Time", "time of the span's enter tracer as a human readable string that includes the nanosecond part."},
	{PT_UINT32, EPF_NONE, PF_DEC, "span.ntags", "Tag Count", "number of tags that this span has."},
	{PT_UINT32, EPF_NONE, PF_DEC, "span.nargs", "Argument Count", "number of arguments that this span has."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "span.tags", "Tags", "dot-separated list of all of the span's tags."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "span.tag", "Tag", "one of the span's tags, specified by 0-based offset, e.g. 'span.tag[1]'. You can use a negative offset to pick elements from the end of the tag list. For example, 'span.tag[-1]' returns the last tag."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "span.args", "Arguments", "comma-separated list of the span's arguments." },
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "span.arg", "Argument", "one of the span arguments, specified by name or by 0-based offset. E.g. 'span.arg.xxx' or 'span.arg[1]'. You can use a negative offset to pick elements from the end of the tag list. For example, 'span.arg[-1]' returns the last argument." },
	{PT_CHARBUF, EPF_NONE, PF_NA, "span.enterargs", "Enter Arguments", "comma-separated list of the span's enter tracer event arguments. For enter tracers, this is the same as evt.args. For exit tracers, this is the evt.args of the corresponding enter tracer." },
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "span.enterarg", "Enter Argument", "one of the span's enter arguments, specified by name or by 0-based offset. For enter tracer events, this is the same as evt.arg. For exit tracer events, this is the evt.arg of the corresponding enter event." },
	{PT_RELTIME, EPF_NONE, PF_DEC, "span.duration", "Duration", "delta between this span's exit tracer event and the enter tracer event."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "span.duration.quantized", "Quantized Duration", "10-base log of the delta between an exit tracer event and the correspondent enter event."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "span.duration.human", "Human-Readable Duration", "delta between this span's exit tracer event and the enter event, as a human readable string (e.g. 10.3ms)."},
	{PT_RELTIME, (filtercheck_field_flags) (EPF_TABLE_ONLY | EPF_ARG_REQUIRED), PF_DEC, "span.duration.fortag", "Duration For Tag", "duration of the span if the number of tags matches the field argument, otherwise 0. For example, span.duration.fortag[1] returns the duration of all the spans with 1 tag, and zero for all the other ones."},
	{PT_UINT64, EPF_TABLE_ONLY, PF_DEC, "span.count", "Span Count", "1 for span exit events."},
	{PT_UINT64, (filtercheck_field_flags) (EPF_TABLE_ONLY | EPF_ARG_REQUIRED), PF_DEC, "span.count.fortag", "Count For Tag", "1 if the span's number of tags matches the field argument, and zero for all the other ones."},
	{PT_UINT64, (filtercheck_field_flags) (EPF_TABLE_ONLY | EPF_ARG_REQUIRED), PF_DEC, "span.childcount.fortag", "Child Count For Tag", "1 if the span's number of tags is greater than the field argument, and zero for all the other ones."},
	{PT_CHARBUF, (filtercheck_field_flags) (EPF_TABLE_ONLY | EPF_ARG_REQUIRED), PF_NA, "span.idtag", "List View ID", "id used by the span list view."},
	{PT_CHARBUF, EPF_TABLE_ONLY, PF_NA, "span.rawtime", "List View Time", "id used by the span list view."},
	{PT_CHARBUF, EPF_TABLE_ONLY, PF_NA, "span.rawparenttime", "List View Parent Time", "id used by the span list view."},
};

sinsp_filter_check_tracer::sinsp_filter_check_tracer()
{
	m_storage = NULL;
	m_info.m_name = "span";
	m_info.m_desc = "Fields used if information about distributed tracing is available.";
	m_info.m_fields = sinsp_filter_check_tracer_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_tracer_fields) / sizeof(sinsp_filter_check_tracer_fields[0]);
	m_converter = new sinsp_filter_check_reference();

	m_storage_size = UESTORAGE_INITIAL_BUFSIZE;
	m_storage = (char*)malloc(m_storage_size);
	if(m_storage == NULL)
	{
		throw sinsp_exception("memory allocation error in sinsp_filter_check_tracer::sinsp_filter_check_tracer");
	}

	m_cargname = NULL;
}

sinsp_filter_check_tracer::~sinsp_filter_check_tracer()
{
	if(m_converter != NULL)
	{
		delete m_converter;
	}

	if(m_storage != NULL)
	{
		free(m_storage);
	}
}

sinsp_filter_check* sinsp_filter_check_tracer::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_tracer();
}

int32_t sinsp_filter_check_tracer::extract_arg(string fldname, string val, OUT const struct ppm_param_info** parinfo)
{
	uint32_t parsed_len = 0;

	//
	// 'arg' and 'resarg' are handled in a custom way
	//
	if(val[fldname.size()] == '[')
	{
		if(parinfo != NULL)
		{
			throw sinsp_exception("tracer field must be expressed explicitly");
		}

		parsed_len = (uint32_t)val.find(']');
		string numstr = val.substr(fldname.size() + 1, parsed_len - fldname.size() - 1);
		m_argid = sinsp_numparser::parsed32(numstr);
		parsed_len++;
	}
	else if(val[fldname.size()] == '.')
	{
		if(fldname == "span.tag")
		{
			throw sinsp_exception("invalid syntax for span.tag");
		}
		else if(fldname == "span.idtag")
		{
			throw sinsp_exception("invalid syntax for span.idtag");
		}

		m_argname = val.substr(fldname.size() + 1);
		m_cargname = m_argname.c_str();
		parsed_len = (uint32_t)(fldname.size() + m_argname.size() + 1);
		m_argid = TEXT_ARG_ID;
	}
	else
	{
		throw sinsp_exception("filter syntax error: " + val);
	}

	return parsed_len;
}

int32_t sinsp_filter_check_tracer::parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering)
{
	int32_t res;
	string val(str);

	//
	// A couple of fields are handled in a custom way
	//
	if(STR_MATCH("span.tag") &&
		!STR_MATCH("span.tags"))
	{
		m_field_id = TYPE_TAG;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("span.tag", val, NULL);
	}
	else if(STR_MATCH("span.arg") &&
		!STR_MATCH("span.args"))
	{
		m_field_id = TYPE_ARG;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("span.arg", val, NULL);
	}
	else if(STR_MATCH("span.enterarg") &&
		!STR_MATCH("span.enterargs"))
	{
		m_field_id = TYPE_ENTERARG;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("span.enterarg", val, NULL);
	}
	else if(STR_MATCH("span.duration.fortag"))
	{
		m_field_id = TYPE_TAGDURATION;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("span.duration.fortag", val, NULL);
	}
	else if(STR_MATCH("span.count.fortag"))
	{
		m_field_id = TYPE_TAGCOUNT;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("span.count.fortag", val, NULL);
	}
	else if(STR_MATCH("span.childcount.fortag"))
	{
		m_field_id = TYPE_TAGCHILDSCOUNT;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("span.childcount.fortag", val, NULL);
	}
	else if(STR_MATCH("span.idtag"))
	{
		m_field_id = TYPE_IDTAG;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("span.idtag", val, NULL);
	}
	else
	{
		res = sinsp_filter_check::parse_field_name(str, alloc_state, needed_for_filtering);
	}

	if(m_field_id == TYPE_DURATION ||
		m_field_id == TYPE_DURATION_QUANTIZED ||
		m_field_id == TYPE_DURATION_HUMAN ||
		m_field_id == TYPE_TAGDURATION ||
		m_field_id == TYPE_ARG ||
		m_field_id == TYPE_ARGS ||
		m_field_id == TYPE_ENTERARG ||
		m_field_id == TYPE_ENTERARGS ||
		m_field_id == TYPE_IDTAG ||
		m_field_id == TYPE_TIME ||
		m_field_id == TYPE_RAWTIME ||
		m_field_id == TYPE_RAWPARENTTIME
		)
	{
		m_inspector->request_tracer_state_tracking();
		m_needs_state_tracking = true;
	}

	return res;
}

uint8_t* sinsp_filter_check_tracer::extract_duration(uint16_t etype, sinsp_tracerparser* eparser, OUT uint32_t* len)
{
	if(etype == PPME_TRACER_X)
	{
		sinsp_partial_tracer* pae = eparser->m_enter_pae;
		if(pae == NULL)
		{
			return NULL;
		}

		m_s64val = eparser->m_exit_pae.m_time - pae->m_time;
		if(m_s64val < 0)
		{
			ASSERT(false);
			m_s64val = 0;
		}

		RETURN_EXTRACT_VAR(m_s64val);
	}
	else
	{
		return NULL;
	}
}

uint8_t* sinsp_filter_check_tracer::extract_args(sinsp_partial_tracer* pae, OUT uint32_t* len)
{
	if(pae == NULL)
	{
		return NULL;
	}

	vector<char*>::iterator nameit;
	vector<char*>::iterator valit;
	vector<uint32_t>::iterator namesit;
	vector<uint32_t>::iterator valsit;

	uint32_t nargs = (uint32_t)pae->m_argnames.size();
	uint32_t encoded_args_len = pae->m_argnames_len + pae->m_argvals_len +
	nargs + nargs + 2;

	if(m_storage_size < encoded_args_len)
	{
		char *new_storage = (char*)realloc(m_storage, encoded_args_len);
		if(new_storage == NULL)
		{
			return NULL;
		}
		m_storage = new_storage;
		m_storage_size = encoded_args_len;
	}

	char* p = m_storage;
	size_t storage_len = 0;

	for(nameit = pae->m_argnames.begin(), valit = pae->m_argvals.begin(),
		namesit = pae->m_argnamelens.begin(), valsit = pae->m_argvallens.begin();
		nameit != pae->m_argnames.end();
		++nameit, ++namesit, ++valit, ++valsit)
	{
		strlcpy(p + storage_len, *nameit, m_storage_size - storage_len);
		storage_len += (*namesit);
		m_storage[storage_len] = '=';
		storage_len++;

		memcpy(p + storage_len, *valit, (*valsit));
		storage_len += (*valsit);
		m_storage[storage_len] = ',';
		storage_len++;
	}

	if (storage_len == 0)
	{
		m_storage[0] = '\0';
	}
	else
	{
		m_storage[storage_len - 1] = '\0';
	}

	RETURN_EXTRACT_CSTR(m_storage);
}

uint8_t* sinsp_filter_check_tracer::extract_arg(sinsp_partial_tracer* pae, OUT uint32_t* len)
{
	char* res = NULL;

	if(pae == NULL)
	{
		return NULL;
	}

	if(m_argid == TEXT_ARG_ID)
	{
		//
		// Argument expressed as name, e.g. span.arg.name.
		// Scan the argname list and find the match.
		//
		uint32_t j;

		for(j = 0; j < pae->m_nargs; j++)
		{
			if(strcmp(m_cargname, pae->m_argnames[j]) == 0)
			{
				res = pae->m_argvals[j];
				break;
			}
		}
	}
	else
	{
		//
		// Argument expressed as id, e.g. span.arg[1].
		// Pick the corresponding value.
		//
		if(m_argid >= 0)
		{
			if(m_argid < (int32_t)pae->m_nargs)
			{
				res = pae->m_argvals[m_argid];
			}
		}
		else
		{
			int32_t id = (int32_t)pae->m_nargs + m_argid;

			if(id >= 0)
			{
				res = pae->m_argvals[id];
			}
		}
	}

	if (res)
	{
		*len = strlen(res);
	}
	return (uint8_t*)res;
}

uint8_t* sinsp_filter_check_tracer::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	sinsp_tracerparser* eparser;
	sinsp_threadinfo* tinfo = evt->get_thread_info();
	uint16_t etype = evt->get_type();

	if(etype != PPME_TRACER_E && etype != PPME_TRACER_X)
	{
		return NULL;
	}

	if(tinfo == NULL)
	{
		return NULL;
	}

	eparser = tinfo->m_tracer_parser;
	if(eparser == NULL)
	{
		return NULL;
	}
	else
	{
		if(m_needs_state_tracking && eparser->m_enter_pae == NULL)
		{
			return NULL;
		}
	}

	switch(m_field_id)
	{
	case TYPE_ID:
		RETURN_EXTRACT_VAR(eparser->m_id);
	case TYPE_TIME:
		{
			sinsp_utils::ts_to_string(evt->get_ts(), &m_strstorage, false, true);
			RETURN_EXTRACT_STRING(m_strstorage);
		}
	case TYPE_NTAGS:
		m_u32val = (uint32_t)eparser->m_tags.size();
		RETURN_EXTRACT_VAR(m_u32val);
	case TYPE_NARGS:
		{
			sinsp_partial_tracer* pae = eparser->m_enter_pae;
			if(pae == NULL)
			{
				return NULL;
			}

			m_u32val = (uint32_t)pae->m_argvals.size();
			RETURN_EXTRACT_VAR(m_u32val);
		}
	case TYPE_TAGS:
		{
			vector<char*>::iterator it;
			vector<uint32_t>::iterator sit;

			uint32_t ntags = (uint32_t)eparser->m_tags.size();
			uint32_t encoded_tags_len = eparser->m_tot_taglens + ntags + 1;

			if(m_storage_size < encoded_tags_len)
			{
				char *new_storage = (char*)realloc(m_storage, encoded_tags_len);
				if(new_storage == NULL)
				{
					return NULL;
				}
				m_storage = new_storage;
				m_storage_size = encoded_tags_len;
			}

			char* p = m_storage;

			for(it = eparser->m_tags.begin(), sit = eparser->m_taglens.begin();
				it != eparser->m_tags.end(); ++it, ++sit)
			{
				memcpy(p, *it, (*sit));
				p += (*sit);
				*p++ = '.';
			}

			if(p != m_storage)
			{
				*--p = 0;
			}
			else
			{
				*p = 0;
			}

			RETURN_EXTRACT_CSTR(m_storage);
		}
	case TYPE_TAG:
		{
			char* res = NULL;

			if(m_argid >= 0)
			{
				if(m_argid < (int32_t)eparser->m_tags.size())
				{
					res = eparser->m_tags[m_argid];
				}
			}
			else
			{
				int32_t id = (int32_t)eparser->m_tags.size() + m_argid;

				if(id >= 0)
				{
					res = eparser->m_tags[id];
				}
			}

			RETURN_EXTRACT_CSTR(res);
		}
	case TYPE_IDTAG:
		{
			m_strstorage = to_string(eparser->m_id);

			if(m_argid >= 0)
			{
				if(m_argid < (int32_t)eparser->m_tags.size())
				{
					m_strstorage += eparser->m_tags[m_argid];
				}
			}
			else
			{
				int32_t id = (int32_t)eparser->m_tags.size() + m_argid;

				if(id >= 0)
				{
					m_strstorage += eparser->m_tags[id];
				}
			}

			RETURN_EXTRACT_STRING(m_strstorage);
		}
	case TYPE_ARGS:
		if(PPME_IS_ENTER(etype))
		{
			return extract_args(eparser->m_enter_pae, len);
		}
		else
		{
			return extract_args(&eparser->m_exit_pae, len);
		}
	case TYPE_ARG:
		if(PPME_IS_ENTER(etype))
		{
			return extract_arg(eparser->m_enter_pae, len);
		}
		else
		{
			return extract_arg(&eparser->m_exit_pae, len);
		}
	case TYPE_ENTERARGS:
		return extract_args(eparser->m_enter_pae, len);
	case TYPE_ENTERARG:
		return extract_arg(eparser->m_enter_pae, len);
	case TYPE_DURATION:
		return (uint8_t*)extract_duration(etype, eparser, len);
	case TYPE_DURATION_HUMAN:
		{
			if(extract_duration(etype, eparser, len) == NULL)
			{
				return NULL;
			}
			else
			{
				m_converter->set_val(PT_RELTIME,
					EPF_NONE,
					(uint8_t*)&m_s64val,
					8,
					0,
					ppm_print_format::PF_DEC);

				m_strstorage = m_converter->tostring_nice(NULL, 0, 1000000000);
			}

			RETURN_EXTRACT_STRING(m_strstorage);
		}
	case TYPE_DURATION_QUANTIZED:
		{
			if(extract_duration(etype, eparser, len) == NULL)
			{
				return NULL;
			}
			else
			{
				uint64_t lat = m_s64val;
				if(lat != 0)
				{
					double lduration = log10((double)lat);

					if(lduration > 11)
					{
						lduration = 11;
					}

					m_s64val = (uint64_t)(lduration * g_screen_w / 11) + 1;

					RETURN_EXTRACT_VAR(m_s64val);
				}
			}

			return NULL;
		}
	case TYPE_TAGDURATION:
		if((int32_t)eparser->m_tags.size() - 1 == m_argid)
		{
			return (uint8_t*)extract_duration(etype, eparser, len);
		}
		else
		{
			return NULL;
		}
	case TYPE_COUNT:
		if(evt->get_type() == PPME_TRACER_X)
		{
			m_s64val = 1;
		}
		else
		{
			m_s64val = 0;
		}

		RETURN_EXTRACT_VAR(m_s64val);
	case TYPE_TAGCOUNT:
		if(PPME_IS_EXIT(evt->get_type()) && (int32_t)eparser->m_tags.size() - 1 == m_argid)
		{
			m_s64val = 1;
		}
		else
		{
			m_s64val = 0;
		}

		RETURN_EXTRACT_VAR(m_s64val);
	case TYPE_TAGCHILDSCOUNT:
		if(PPME_IS_EXIT(evt->get_type()) && (int32_t)eparser->m_tags.size() > m_argid + 1)
		{
			m_s64val = 1;
		}
		else
		{
			m_s64val = 0;
		}

		RETURN_EXTRACT_VAR(m_s64val);
	case TYPE_RAWTIME:
		{
			m_strstorage = to_string(eparser->m_enter_pae->m_time);
			RETURN_EXTRACT_STRING(m_strstorage);
		}
	case TYPE_RAWPARENTTIME:
		{
			sinsp_partial_tracer* pepae = eparser->find_parent_enter_pae();

			if(pepae == NULL)
			{
				return NULL;
			}

			m_strstorage = to_string(pepae->m_time);
			RETURN_EXTRACT_STRING(m_strstorage);
		}
	default:
		ASSERT(false);
		break;
	}

	return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_tracer implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_evtin_fields[] =
{
	{ PT_INT64, EPF_NONE, PF_ID, "evtin.span.id", "In Span ID", "accepts all the events that are between the enter and exit tracers of the spans with the given ID and are generated by the same thread that generated the tracers." },
	{ PT_UINT32, EPF_NONE, PF_DEC, "evtin.span.ntags", "In Span Tag Count", "accepts all the events that are between the enter and exit tracers of the spans with the given number of tags and are generated by the same thread that generated the tracers." },
	{ PT_UINT32, EPF_NONE, PF_DEC, "evtin.span.nargs", "In Span Argument Count", "accepts all the events that are between the enter and exit tracers of the spans with the given number of arguments and are generated by the same thread that generated the tracers." },
	{ PT_CHARBUF, EPF_NONE, PF_NA, "evtin.span.tags", "In Span Tags", "accepts all the events that are between the enter and exit tracers of the spans with the given tags and are generated by the same thread that generated the tracers." },
	{ PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "evtin.span.tag", "In Span Tag", "accepts all the events that are between the enter and exit tracers of the spans with the given tag and are generated by the same thread that generated the tracers. See the description of span.tag for information about the syntax accepted by this field." },
	{ PT_CHARBUF, EPF_NONE, PF_NA, "evtin.span.args", "In Span Arguments", "accepts all the events that are between the enter and exit tracers of the spans with the given arguments and are generated by the same thread that generated the tracers." },
	{ PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "evtin.span.arg", "In Span Argument", "accepts all the events that are between the enter and exit tracers of the spans with the given argument and are generated by the same thread that generated the tracers. See the description of span.arg for information about the syntax accepted by this field." },
	{ PT_INT64, EPF_NONE, PF_ID, "evtin.span.p.id", "In Parent ID", "same as evtin.span.id, but also accepts events generated by other threads in the same process that produced the span." },
	{ PT_UINT32, EPF_NONE, PF_DEC, "evtin.span.p.ntags", "In Parent Tag Count", "same as evtin.span.ntags, but also accepts events generated by other threads in the same process that produced the span." },
	{ PT_UINT32, EPF_NONE, PF_DEC, "evtin.span.p.nargs", "In Parent Argument Count", "same as evtin.span.nargs, but also accepts events generated by other threads in the same process that produced the span." },
	{ PT_CHARBUF, EPF_NONE, PF_NA, "evtin.span.p.tags", "In Parent Tags", "same as evtin.span.tags, but also accepts events generated by other threads in the same process that produced the span." },
	{ PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "evtin.span.p.tag", "In Parent Tag", "same as evtin.span.tag, but also accepts events generated by other threads in the same process that produced the span." },
	{ PT_CHARBUF, EPF_NONE, PF_NA, "evtin.span.p.args", "In Parent Arguments", "same as evtin.span.args, but also accepts events generated by other threads in the same process that produced the span." },
	{ PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "evtin.span.p.arg", "In Parent Argument", "same as evtin.span.arg, but also accepts events generated by other threads in the same process that produced the span." },
	{ PT_INT64, EPF_NONE, PF_ID, "evtin.span.s.id", "In Script ID", "same as evtin.span.id, but also accepts events generated by the script that produced the span, i.e. by the processes whose parent PID is the same as the one of the process generating the span." },
	{ PT_UINT32, EPF_NONE, PF_DEC, "evtin.span.s.ntags", "In Script Tag Count", "same as evtin.span.id, but also accepts events generated by the script that produced the span, i.e. by the processes whose parent PID is the same as the one of the process generating the span." },
	{ PT_UINT32, EPF_NONE, PF_DEC, "evtin.span.s.nargs", "In Script Argument Count", "same as evtin.span.id, but also accepts events generated by the script that produced the span, i.e. by the processes whose parent PID is the same as the one of the process generating the span." },
	{ PT_CHARBUF, EPF_NONE, PF_NA, "evtin.span.s.tags", "In Script Tags", "same as evtin.span.id, but also accepts events generated by the script that produced the span, i.e. by the processes whose parent PID is the same as the one of the process generating the span." },
	{ PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "evtin.span.s.tag", "In Script Tag", "same as evtin.span.id, but also accepts events generated by the script that produced the span, i.e. by the processes whose parent PID is the same as the one of the process generating the span." },
	{ PT_CHARBUF, EPF_NONE, PF_NA, "evtin.span.s.args", "In Script Arguments", "same as evtin.span.id, but also accepts events generated by the script that produced the span, i.e. by the processes whose parent PID is the same as the one of the process generating the span." },
	{ PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "evtin.span.s.arg", "In Script Argument", "same as evtin.span.id, but also accepts events generated by the script that produced the span, i.e. by the processes whose parent PID is the same as the one of the process generating the span." },
	{ PT_INT64, EPF_NONE, PF_ID, "evtin.span.m.id", "In Machine ID", "same as evtin.span.id, but accepts all the events generated on the machine during the span, including other threads and other processes." },
	{ PT_UINT32, EPF_NONE, PF_DEC, "evtin.span.m.ntags", "In Machine Tag Count", "same as evtin.span.id, but accepts all the events generated on the machine during the span, including other threads and other processes." },
	{ PT_UINT32, EPF_NONE, PF_DEC, "evtin.span.m.nargs", "In Machine Argument Count", "same as evtin.span.id, but accepts all the events generated on the machine during the span, including other threads and other processes." },
	{ PT_CHARBUF, EPF_NONE, PF_NA, "evtin.span.m.tags", "In Machine Tags", "same as evtin.span.id, but accepts all the events generated on the machine during the span, including other threads and other processes." },
	{ PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "evtin.span.m.tag", "In Machine Tag", "same as evtin.span.id, but accepts all the events generated on the machine during the span, including other threads and other processes." },
	{ PT_CHARBUF, EPF_NONE, PF_NA, "evtin.span.m.args", "In Machine Arguments", "same as evtin.span.id, but accepts all the events generated on the machine during the span, including other threads and other processes." },
	{ PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "evtin.span.m.arg", "In Machine Argument", "same as evtin.span.id, but accepts all the events generated on the machine during the span, including other threads and other processes." },
};

sinsp_filter_check_evtin::sinsp_filter_check_evtin()
{
	m_is_compare = false;
	m_info.m_name = "evtin";
	m_info.m_desc = "Fields used if information about distributed tracing is available.";
	m_info.m_fields = sinsp_filter_check_evtin_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_evtin_fields) / sizeof(sinsp_filter_check_evtin_fields[0]);
	m_u64val = 0;
	m_converter = new sinsp_filter_check_reference();

	m_storage_size = UESTORAGE_INITIAL_BUFSIZE;
	m_storage = (char*)malloc(m_storage_size);
	if(m_storage == NULL)
	{
		throw sinsp_exception("memory allocation error in sinsp_filter_check_appevt::sinsp_filter_check_evtin");
	}

	m_cargname = NULL;
}

sinsp_filter_check_evtin::~sinsp_filter_check_evtin()
{
	if(m_storage != NULL)
	{
		free(m_storage);
	}

	if(m_converter != NULL)
	{
		delete m_converter;
	}
}

int32_t sinsp_filter_check_evtin::extract_arg(string fldname, string val)
{
	uint32_t parsed_len = 0;

	//
	// 'arg' and 'resarg' are handled in a custom way
	//
	if(val[fldname.size()] == '[')
	{
		parsed_len = (uint32_t)val.find(']');
		string numstr = val.substr(fldname.size() + 1, parsed_len - fldname.size() - 1);

		m_argid = sinsp_numparser::parsed32(numstr);

		parsed_len++;
	}
	else if(val[fldname.size()] == '.')
	{
		const struct ppm_param_info* pi =
			sinsp_utils::find_longest_matching_evt_param(val.substr(fldname.size() + 1));

		if(pi == NULL)
		{
			throw sinsp_exception("unknown event argument " + val.substr(fldname.size() + 1));
		}

		m_argname = pi->name;
		parsed_len = (uint32_t)(fldname.size() + strlen(pi->name) + 1);
		m_argid = -1;
	}
	else
	{
		throw sinsp_exception("filter syntax error: " + val);
	}

	return parsed_len;
}

int32_t sinsp_filter_check_evtin::parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering)
{
	int32_t res;
	string val(str);

	//
	// All of the fields require state tracking
	//
	m_inspector->request_tracer_state_tracking();

	//
	// A couple of fields are handled in a custom way
	//
	if(STR_MATCH("evtin.span.tag") &&
		!STR_MATCH("evtin.span.tags"))
	{
		m_field_id = TYPE_TAG;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("evtin.span.tag", val);
	}
	else if(STR_MATCH("evtin.span.arg") &&
		!STR_MATCH("evtin.span.args"))
	{
		m_field_id = TYPE_ARG;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("evtin.span.arg", val);
	}
	else if(STR_MATCH("evtin.span.p.tag") &&
		!STR_MATCH("evtin.span.p.tags"))
	{
		m_field_id = TYPE_P_TAG;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("evtin.span.p.tag", val);
	}
	else if(STR_MATCH("evtin.span.p.arg") &&
		!STR_MATCH("evtin.span.p.args"))
	{
		m_field_id = TYPE_P_ARG;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("evtin.span.p.arg", val);
	}
	else if(STR_MATCH("evtin.span.s.tag") &&
		!STR_MATCH("evtin.span.s.tags"))
	{
		m_field_id = TYPE_S_TAG;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("evtin.span.s.tag", val);
	}
	else if(STR_MATCH("evtin.span.s.arg") &&
		!STR_MATCH("evtin.span.s.args"))
	{
		m_field_id = TYPE_S_ARG;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("evtin.span.s.arg", val);
	}
	else if(STR_MATCH("evtin.span.m.tag") &&
		!STR_MATCH("evtin.span.m.tags"))
	{
		m_field_id = TYPE_M_TAG;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("evtin.span.m.tag", val);
	}
	else if(STR_MATCH("evtin.span.m.arg") &&
		!STR_MATCH("evtin.span.m.args"))
	{
		m_field_id = TYPE_M_ARG;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg("evtin.span.m.arg", val);
	}
	else
	{
		res = sinsp_filter_check::parse_field_name(str, alloc_state, needed_for_filtering);
	}

	return res;
}

sinsp_filter_check* sinsp_filter_check_evtin::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_evtin();
}

inline uint8_t* sinsp_filter_check_evtin::extract_tracer(sinsp_evt *evt, sinsp_partial_tracer* pae, OUT uint32_t* len)
{
	ASSERT(pae);
	uint32_t field_id = m_field_id;

	if(field_id >= TYPE_ID && field_id <= TYPE_ARG)
	{
		//
		// If this is a thread-related field, reject anything that doesn't come from the same thread
		//
		if(static_cast<int64_t>(pae->m_tid) != evt->get_thread_info()->m_tid)
		{
			return NULL;
		}
	}
	else if(field_id >= TYPE_P_ID && field_id <= TYPE_P_ARG)
	{
		//
		// If this is a *.p.* field, reject anything that doesn't come from the same process
		//
		sinsp_threadinfo* tinfo = m_inspector->get_thread_ref(pae->m_tid).get();

		if(tinfo)
		{
			if(tinfo->m_tid != evt->get_thread_info()->m_tid)
			{
				return NULL;
			}
		}
		else
		{
			return NULL;
		}

		field_id -= TYPE_P_ID;
	}
	else if(field_id >= TYPE_S_ID && field_id <= TYPE_S_ARG)
	{
		//
		// If this is a *.p.* field, reject anything that doesn't share the same parent
		//
		sinsp_threadinfo* tinfo = m_inspector->get_thread_ref(pae->m_tid).get();

		if(tinfo)
		{
			if(tinfo->m_pid != evt->get_thread_info()->m_ptid)
			{
				return NULL;
			}
		}
		else
		{
			return NULL;
		}

		field_id -= TYPE_S_ID;
	}
	else
	{
		field_id -= TYPE_M_ID;
	}

	switch(field_id)
	{
	case TYPE_ID:
		RETURN_EXTRACT_VAR(pae->m_id);
	case TYPE_NTAGS:
		m_u32val = (uint32_t)pae->m_tags.size();
		RETURN_EXTRACT_VAR(m_u32val);
	case TYPE_NARGS:
		m_u32val = (uint32_t)pae->m_argvals.size();
		RETURN_EXTRACT_VAR(m_u32val);
	case TYPE_TAGS:
	{
		vector<char*>::iterator it;
		vector<uint32_t>::iterator sit;

		uint32_t encoded_tags_len = pae->m_tags_len + pae->m_ntags + 1;

		if(m_storage_size < encoded_tags_len)
		{
			char *new_storage = (char*)realloc(m_storage, encoded_tags_len);
			if(new_storage == NULL)
			{
				return NULL;
			}
			m_storage = new_storage;
			m_storage_size = encoded_tags_len;
		}

		char* p = m_storage;

		for(it = pae->m_tags.begin(), sit = pae->m_taglens.begin();
		it != pae->m_tags.end(); ++it, ++sit)
		{
			memcpy(p, *it, (*sit));
			p += (*sit);
			*p++ = '.';
		}

		if(p != m_storage)
		{
			*--p = 0;
		}
		else
		{
			*p = 0;
		}

		RETURN_EXTRACT_CSTR(m_storage);
	}
	case TYPE_TAG:
	{
		char* val = NULL;

		if(m_argid >= 0)
		{
			if(m_argid < (int32_t)pae->m_ntags)
			{
				val = pae->m_tags[m_argid];
			}
		}
		else
		{
			int32_t id = (int32_t)pae->m_ntags + m_argid;

			if(id >= 0)
			{
				val = pae->m_tags[id];
			}
		}

		RETURN_EXTRACT_CSTR(val);
	}
	case TYPE_ARGS:
	{
		vector<char*>::iterator nameit;
		vector<char*>::iterator valit;
		vector<uint32_t>::iterator namesit;
		vector<uint32_t>::iterator valsit;

		uint32_t nargs = (uint32_t)pae->m_argnames.size();
		uint32_t encoded_args_len = pae->m_argnames_len + pae->m_argvals_len +
			nargs + nargs + 2;

		if(m_storage_size < encoded_args_len)
		{
			char *new_storage = (char*)realloc(m_storage, encoded_args_len);
			if(new_storage == NULL)
			{
				return NULL;
			}
			m_storage = new_storage;
			m_storage_size = encoded_args_len;
		}

		char* p = m_storage;
		size_t storage_len = 0;

		for(nameit = pae->m_argnames.begin(), valit = pae->m_argvals.begin(),
			namesit = pae->m_argnamelens.begin(), valsit = pae->m_argvallens.begin();
			nameit != pae->m_argnames.end();
			++nameit, ++namesit, ++valit, ++valsit)
		{
			strlcpy(p + storage_len, *nameit, m_storage_size - storage_len);
			storage_len += (*namesit);
			m_storage[storage_len] = ':';
			storage_len++;

			memcpy(p + storage_len, *valit, (*valsit));
			storage_len += (*valsit);
			m_storage[storage_len] = ',';
			storage_len++;
		}

		if (storage_len == 0)
		{
			m_storage[0] = '\0';
		}
		else
		{
			m_storage[storage_len - 1] = '\0';
		}

		RETURN_EXTRACT_CSTR(m_storage);
	}
	case TYPE_ARG:
	{
		char* val = NULL;

		if(m_argid == TEXT_ARG_ID)
		{
			//
			// Argument expressed as name, e.g. evtin.span.arg.name.
			// Scan the argname list and find the match.
			//
			uint32_t j;

			for(j = 0; j < pae->m_nargs; j++)
			{
				if(strcmp(m_cargname, pae->m_argnames[j]) == 0)
				{
					val = pae->m_argvals[j];
					break;
				}
			}
		}
		else
		{
			//
			// Argument expressed as id, e.g. evtin.span.arg[1].
			// Pick the corresponding value.
			//
			if(m_argid >= 0)
			{
				if(m_argid < (int32_t)pae->m_nargs)
				{
					val = pae->m_argvals[m_argid];
				}
			}
			else
			{
				int32_t id = (int32_t)pae->m_nargs + m_argid;

				if(id >= 0)
				{
					val = pae->m_argvals[id];
				}
			}
		}

		RETURN_EXTRACT_CSTR(val);
	}
	default:
		ASSERT(false);
		break;
	}

	return NULL;
}

uint8_t* sinsp_filter_check_evtin::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	list<sinsp_partial_tracer*>* partial_tracers_list = &m_inspector->m_partial_tracers_list;
	list<sinsp_partial_tracer*>::iterator it;
	uint16_t etype = evt->get_type();

	//
	// Tracer events are excluded
	//
	if(etype == PPME_TRACER_E || etype == PPME_TRACER_X)
	{
		return NULL;
	}

	//
	// Events without thread information are excluded
	//
	sinsp_threadinfo* tinfo = evt->get_thread_info();
	if(tinfo == NULL || tinfo->m_tracer_parser == NULL)
	{
		return NULL;
	}

	//
	// Scan the list and see if there's a match
	//
	for(it = partial_tracers_list->begin(); it != partial_tracers_list->end(); ++it)
	{
		uint8_t* res = extract_tracer(evt, *it, len);
		if(res != NULL)
		{
			return res;
		}
	}

	return NULL;
}

inline bool sinsp_filter_check_evtin::compare_tracer(sinsp_evt *evt, sinsp_partial_tracer* pae)
{
	uint32_t len;
	uint8_t* res = extract_tracer(evt, pae, &len);

	if(res == NULL)
	{
		return false;
	}

	if(flt_compare(m_cmpop, m_info.m_fields[m_field_id].m_type,
		res) == true)
	{
		return true;
	}
	else
	{
		return false;
	}
}

bool sinsp_filter_check_evtin::compare(sinsp_evt *evt)
{
	bool res;

	m_is_compare = true;

	list<sinsp_partial_tracer*>* partial_tracers_list = &m_inspector->m_partial_tracers_list;
	list<sinsp_partial_tracer*>::iterator it;
	uint16_t etype = evt->get_type();

	sinsp_threadinfo* tinfo = evt->get_thread_info();
	if(tinfo == NULL)
	{
		res = false;
		goto fcec_end;
	}

	//
	// Scan the list and see if there's a match
	//
	for(it = partial_tracers_list->begin(); it != partial_tracers_list->end(); ++it)
	{
		if(compare_tracer(evt, *it) == true)
		{
			if(etype == PPME_TRACER_E && *it == tinfo->m_tracer_parser->m_enter_pae)
			{
				res = false;
				goto fcec_end;
			}

			res = true;
			goto fcec_end;
		}
	}

	//
	// For PPME_TRACER_X events, it's possible that the pae is already returned to the pool.
	// Get it from the parser.
	//
	if(etype == PPME_TRACER_X)
	{
		sinsp_tracerparser* eparser = tinfo->m_tracer_parser;

		if(eparser == NULL)
		{
			ASSERT(false);
			res = false;
			goto fcec_end;
		}

		if(eparser->m_enter_pae == NULL)
		{
			res = false;
			goto fcec_end;
		}

		if(compare_tracer(evt, eparser->m_enter_pae) == true)
		{
			res = true;
			goto fcec_end;
		}
	}

	res = false;

fcec_end:
	m_is_compare = false;

	return res;
}

///////////////////////////////////////////////////////////////////////////////
// rawstring_check implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info rawstring_check_fields[] =
{
	{PT_CHARBUF, EPF_NONE, PF_NA, "NA", "NA", "INTERNAL."},
};

rawstring_check::rawstring_check(string text)
{
	m_field = rawstring_check_fields;
	m_field_id = 0;
	set_text(text);
}

sinsp_filter_check* rawstring_check::allocate_new()
{
	ASSERT(false);
	return NULL;
}

void rawstring_check::set_text(string text)
{
	m_text_len = (uint32_t)text.size();
	m_text = text;
}

int32_t rawstring_check::parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering)
{
	ASSERT(false);
	return -1;
}

uint8_t* rawstring_check::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = m_text_len;
	return (uint8_t*)m_text.c_str();
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_syslog implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_syslog_fields[] =
{
	{PT_CHARBUF, EPF_NONE, PF_NA, "syslog.facility.str", "Facility", "facility as a string."},
	{PT_UINT32, EPF_NONE, PF_DEC, "syslog.facility", "Numeric Facility", "facility as a number (0-23)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "syslog.severity.str", "Severity", "severity as a string. Can have one of these values: emerg, alert, crit, err, warn, notice, info, debug"},
	{PT_UINT32, EPF_NONE, PF_DEC, "syslog.severity", "Numeric Severity", "severity as a number (0-7)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "syslog.message", "Message", "message sent to syslog."},
};

sinsp_filter_check_syslog::sinsp_filter_check_syslog()
{
	m_info.m_name = "syslog";
	m_info.m_desc = "Content of Syslog messages.";
	m_info.m_fields = sinsp_filter_check_syslog_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_syslog_fields) / sizeof(sinsp_filter_check_syslog_fields[0]);
	m_decoder = NULL;
}

sinsp_filter_check* sinsp_filter_check_syslog::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_syslog();
}

int32_t sinsp_filter_check_syslog::parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering)
{
	int32_t res = sinsp_filter_check::parse_field_name(str, alloc_state, needed_for_filtering);
	if(res != -1)
	{
		m_decoder = (sinsp_decoder_syslog*)m_inspector->require_protodecoder("syslog");
	}

	return res;
}

uint8_t* sinsp_filter_check_syslog::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	const char *str;
	ASSERT(m_decoder != NULL);
	if(!m_decoder->is_data_valid())
	{
		return NULL;
	}

	switch(m_field_id)
	{
	case TYPE_FACILITY:
		RETURN_EXTRACT_VAR(m_decoder->m_facility);
	case TYPE_FACILITY_STR:
		str = m_decoder->get_facility_str();
		RETURN_EXTRACT_CSTR(str);
	case TYPE_SEVERITY:
		RETURN_EXTRACT_VAR(m_decoder->m_severity);
	case TYPE_SEVERITY_STR:
		str = m_decoder->get_severity_str();
		RETURN_EXTRACT_CSTR(str);
	case TYPE_MESSAGE:
		RETURN_EXTRACT_STRING(m_decoder->m_msg);
	default:
		ASSERT(false);
		return NULL;
	}
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_container implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_container_fields[] =
{
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.id", "Container ID", "The truncated container id (first 12 characters)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.name", "Container Name", "The container name."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.image", "Image Name", "The container image name (e.g. falcosecurity/falco:latest for docker)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.image.id", "Image ID", "The container image id (e.g. 6f7e2741b66b)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.type", "Type", "The container type, eg: docker or rkt"},
	{PT_BOOL, EPF_NONE, PF_NA, "container.privileged", "Privileged", "'true' for containers running as privileged, false otherwise"},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.mounts", "Mounts", "A space-separated list of mount information. Each item in the list has the format <source>:<dest>:<mode>:<rdrw>:<propagation>"},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "container.mount", "Mount", "Information about a single mount, specified by number (e.g. container.mount[0]) or mount source (container.mount[/usr/local]). The pathname can be a glob (container.mount[/usr/local/*]), in which case the first matching mount will be returned. The information has the format <source>:<dest>:<mode>:<rdrw>:<propagation>. If there is no mount with the specified index or matching the provided source, returns the string \"none\" instead of a NULL value."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "container.mount.source", "Mount Source", "The mount source, specified by number (e.g. container.mount.source[0]) or mount destination (container.mount.source[/host/lib/modules]). The pathname can be a glob."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "container.mount.dest", "Mount Destination", "The mount destination, specified by number (e.g. container.mount.dest[0]) or mount source (container.mount.dest[/lib/modules]). The pathname can be a glob."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "container.mount.mode", "Mount Mode", "The mount mode, specified by number (e.g. container.mount.mode[0]) or mount source (container.mount.mode[/usr/local]). The pathname can be a glob."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "container.mount.rdwr", "Mount Read/Write", "The mount rdwr value, specified by number (e.g. container.mount.rdwr[0]) or mount source (container.mount.rdwr[/usr/local]). The pathname can be a glob."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "container.mount.propagation", "Mount Propagation", "The mount propagation value, specified by number (e.g. container.mount.propagation[0]) or mount source (container.mount.propagation[/usr/local]). The pathname can be a glob."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.image.repository", "Repository", "The container image repository (e.g. falcosecurity/falco)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.image.tag", "Image Tag", "The container image tag (e.g. stable, latest)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.image.digest", "Registry Digest", "The container image registry digest (e.g. sha256:d977378f890d445c15e51795296e4e5062f109ce6da83e0a355fc4ad8699d27)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.healthcheck", "Health Check", "The container's health check. Will be the null value (\"N/A\") if no healthcheck configured, \"NONE\" if configured but explicitly not created, and the healthcheck command line otherwise"},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.liveness_probe", "Liveness", "The container's liveness probe. Will be the null value (\"N/A\") if no liveness probe configured, the liveness probe command line otherwise"},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.readiness_probe", "Readiness", "The container's readiness probe. Will be the null value (\"N/A\") if no readiness probe configured, the readiness probe command line otherwise"},
	{PT_UINT64, EPF_NONE, PF_DEC, "container.start_ts", "Container start", "Container start as epoch timestamp in nanoseconds based on proc.pidns_init_start_ts."},
	{PT_RELTIME, EPF_NONE, PF_DEC, "container.duration", "Number of nanoseconds since container.start_ts", "Number of nanoseconds since container.start_ts."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.ip", "Container ip address", "The container's / pod's primary ip address as retrieved from the container engine. Only ipv4 addresses are tracked. Consider container.cni.json (CRI use case) for logging ip addresses for each network interface."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "container.cni.json", "Container's / pod's CNI result json", "The container's / pod's CNI result field from the respective pod status info. It contains ip addresses for each network interface exposed as unparsed escaped JSON string. Supported for CRI container engine (containerd, cri-o runtimes), optimized for containerd (some non-critical JSON keys removed). Useful for tracking ips (ipv4 and ipv6, dual-stack support) for each network interface (multi-interface support)."},
};

sinsp_filter_check_container::sinsp_filter_check_container()
{
	m_info.m_name = "container";
	m_info.m_desc = "Container information. If the event is not happening inside a container, both id and name will be set to 'host'.";
	m_info.m_fields = sinsp_filter_check_container_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_container_fields) / sizeof(sinsp_filter_check_container_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;
}

sinsp_filter_check* sinsp_filter_check_container::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_container();
}

int32_t sinsp_filter_check_container::extract_arg(const string &val, size_t basepos)
{
	size_t start = val.find_first_of('[', basepos);
	if(start == string::npos)
	{
		throw sinsp_exception("filter syntax error: " + val);
	}

	size_t end = val.find_first_of(']', start);
	if(end == string::npos)
	{
		throw sinsp_exception("filter syntax error: " + val);
	}

	string numstr = val.substr(start + 1, end-start-1);
	try
	{
		m_argid = sinsp_numparser::parsed32(numstr);
	}
	catch (const sinsp_exception& e)
	{
		if(strstr(e.what(), "is not a valid number") == NULL)
		{
			throw;
		}

		m_argid = -1;
		m_argstr = numstr;
	}

	return end+1;
}

const std::string &sinsp_filter_check_container::get_argstr()
{
	return m_argstr;
}

int32_t sinsp_filter_check_container::parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering)
{
	string val(str);
	int32_t res = 0;

	size_t basepos = sizeof("container.mount");

	// container.mount. fields allow for indexing by number or source/dest mount path.
	if(val.find("container.mount.") == 0)
	{
		// Note--basepos includes the trailing null, which is
		// equivalent to the trailing '.' here.
		if(val.find("source", basepos) == basepos)
		{
			m_field_id = TYPE_CONTAINER_MOUNT_SOURCE;
		}
		else if(val.find("dest", basepos) == basepos)
		{
			m_field_id = TYPE_CONTAINER_MOUNT_DEST;
		}
		else if(val.find("mode", basepos) == basepos)
		{
			m_field_id = TYPE_CONTAINER_MOUNT_MODE;
		}
		else if(val.find("rdwr", basepos) == basepos)
		{
			m_field_id = TYPE_CONTAINER_MOUNT_RDWR;
		}
		else if(val.find("propagation", basepos) == basepos)
		{
			m_field_id = TYPE_CONTAINER_MOUNT_PROPAGATION;
		}
		else
		{
			throw sinsp_exception("filter syntax error: " + val);
		}
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg(val, basepos);
	}
	else if (val.find("container.mount") == 0 &&
		 val[basepos-1] != 's')
	{
		m_field_id = TYPE_CONTAINER_MOUNT;
		m_field = &m_info.m_fields[m_field_id];

		res = extract_arg(val, basepos-1);
	}
	else
	{
		res = sinsp_filter_check::parse_field_name(str, alloc_state, needed_for_filtering);
	}

	return res;
}


uint8_t* sinsp_filter_check_container::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	sinsp_threadinfo* tinfo = evt->get_thread_info();
	if(tinfo == NULL)
	{
		return NULL;
	}

	sinsp_container_info::ptr_t container_info = NULL;
	bool is_host = true;

	if(!tinfo->m_container_id.empty())
	{
		is_host = false;
		container_info = m_inspector->m_container_manager.get_container(tinfo->m_container_id);
	}

	switch(m_field_id)
	{
	case TYPE_CONTAINER_ID:
		if(is_host)
		{
			m_tstr = "host";
		}
		else
		{
			m_tstr = tinfo->m_container_id;
		}

		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_CONTAINER_NAME:
		if(is_host)
		{
			m_tstr = "host";
		}
		else
		{
			if(!container_info)
			{
				return NULL;
			}

			if(container_info->m_name.empty())
			{
				return NULL;
			}

			m_tstr = container_info->m_name;
		}

		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_CONTAINER_IMAGE:
		if(is_host)
		{
			return NULL;
		}
		else
		{
			if(!container_info)
			{
				return NULL;
			}

			if(container_info->m_image.empty())
			{
				return NULL;
			}

			m_tstr = container_info->m_image;
		}

		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_CONTAINER_IMAGE_ID:
	case TYPE_CONTAINER_IMAGE_REPOSITORY:
	case TYPE_CONTAINER_IMAGE_TAG:
	case TYPE_CONTAINER_IMAGE_DIGEST:
		if(is_host)
		{
			return NULL;
		}
		else
		{
			if(!container_info)
			{
				return NULL;
			}

			const string *field;
			switch(m_field_id)
			{
			case TYPE_CONTAINER_IMAGE_ID:
				field = &container_info->m_imageid;
				break;
			case TYPE_CONTAINER_IMAGE_REPOSITORY:
				field = &container_info->m_imagerepo;
				break;
			case TYPE_CONTAINER_IMAGE_TAG:
				field = &container_info->m_imagetag;
				break;
			case TYPE_CONTAINER_IMAGE_DIGEST:
				field = &container_info->m_imagedigest;
				break;
			default:
				break;
			}

			if(field->empty())
			{
				return NULL;
			}

			m_tstr = *field;
		}

		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_CONTAINER_TYPE:
		if(is_host)
		{
			m_tstr = "host";
		}
		else
		{
			if(!container_info)
			{
				return NULL;
			}
			switch(container_info->m_type)
			{
			case sinsp_container_type::CT_DOCKER:
				m_tstr = "docker";
				break;
			case sinsp_container_type::CT_LXC:
				m_tstr = "lxc";
				break;
			case sinsp_container_type::CT_LIBVIRT_LXC:
				m_tstr = "libvirt-lxc";
				break;
			case sinsp_container_type::CT_MESOS:
				m_tstr = "mesos";
				break;
			case sinsp_container_type::CT_CRI:
				m_tstr = "cri";
				break;
			case sinsp_container_type::CT_CONTAINERD:
				m_tstr = "containerd";
				break;
			case sinsp_container_type::CT_CRIO:
				m_tstr = "cri-o";
				break;
			case sinsp_container_type::CT_RKT:
				m_tstr = "rkt";
				break;
			case sinsp_container_type::CT_BPM:
				m_tstr = "bpm";
				break;
			default:
				ASSERT(false);
				break;
			}
		}
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_CONTAINER_PRIVILEGED:
		if(is_host)
		{
			return NULL;
		}
		else
		{
			if(!container_info)
			{
				return NULL;
			}

			// Only return a true/false value for
			// container types where we really know the
			// privileged status.
			if (!is_docker_compatible(container_info->m_type))
			{
				return NULL;
			}

			m_u32val = (container_info->m_privileged ? 1 : 0);
		}

		RETURN_EXTRACT_VAR(m_u32val);
		break;
	case TYPE_CONTAINER_MOUNTS:
		if(is_host)
		{
			return NULL;
		}
		else
		{
			if(!container_info)
			{
				return NULL;
			}

			m_tstr = "";
			bool first = true;
			for(auto &mntinfo : container_info->m_mounts)
			{
				if(first)
				{
					first = false;
				}
				else
				{
					m_tstr += ",";
				}

				m_tstr += mntinfo.to_string();
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}

		break;
	case TYPE_CONTAINER_MOUNT:
		if(is_host)
		{
			return NULL;
		}
		else
		{
			if(!container_info)
			{
				return NULL;
			}

			const sinsp_container_info::container_mount_info *mntinfo;

			if(m_argid != -1)
			{
				mntinfo = container_info->mount_by_idx(m_argid);
			}
			else
			{
				mntinfo = container_info->mount_by_source(m_argstr);
			}

			if(!mntinfo)
			{
				return NULL;
			}
			else
			{
				m_tstr = mntinfo->to_string();
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}

		break;
	case TYPE_CONTAINER_MOUNT_SOURCE:
	case TYPE_CONTAINER_MOUNT_DEST:
	case TYPE_CONTAINER_MOUNT_MODE:
	case TYPE_CONTAINER_MOUNT_RDWR:
	case TYPE_CONTAINER_MOUNT_PROPAGATION:
		if(is_host)
		{
			return NULL;
		}
		else
		{
			if(!container_info)
			{
				return NULL;
			}

			const sinsp_container_info::container_mount_info *mntinfo;

			if(m_argid != -1)
			{
				mntinfo = container_info->mount_by_idx(m_argid);
			}
			else
			{
				if (m_field_id == TYPE_CONTAINER_MOUNT_SOURCE)
				{
					mntinfo = container_info->mount_by_dest(m_argstr);
				}
				else
				{
					mntinfo = container_info->mount_by_source(m_argstr);
				}
			}

			if(!mntinfo)
			{
				return NULL;
			}

			switch (m_field_id)
			{
			case TYPE_CONTAINER_MOUNT_SOURCE:
				m_tstr = mntinfo->m_source;
				break;
			case TYPE_CONTAINER_MOUNT_DEST:
				m_tstr = mntinfo->m_dest;
				break;
			case TYPE_CONTAINER_MOUNT_MODE:
				m_tstr = mntinfo->m_mode;
				break;
			case TYPE_CONTAINER_MOUNT_RDWR:
				m_tstr = (mntinfo->m_rdwr ? "true" : "false");
				break;
			case TYPE_CONTAINER_MOUNT_PROPAGATION:
				m_tstr = mntinfo->m_propagation;
				break;
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	case TYPE_CONTAINER_HEALTHCHECK:
	case TYPE_CONTAINER_LIVENESS_PROBE:
	case TYPE_CONTAINER_READINESS_PROBE:
		if(is_host)
		{
			return NULL;
		}
		else
		{
			if(!container_info)
			{
				return NULL;
			}

			for(auto &probe : container_info->m_health_probes)
			{
				if((m_field_id == TYPE_CONTAINER_HEALTHCHECK &&
				    probe.m_probe_type == sinsp_container_info::container_health_probe::PT_HEALTHCHECK) ||
				   (m_field_id == TYPE_CONTAINER_LIVENESS_PROBE &&
				    probe.m_probe_type == sinsp_container_info::container_health_probe::PT_LIVENESS_PROBE) ||
				   (m_field_id == TYPE_CONTAINER_READINESS_PROBE &&
				    probe.m_probe_type == sinsp_container_info::container_health_probe::PT_READINESS_PROBE))
				{
					m_tstr = probe.m_health_probe_exe;

					for(auto &arg : probe.m_health_probe_args)
					{
						m_tstr += " ";
						m_tstr += arg;
					}

					RETURN_EXTRACT_STRING(m_tstr);
				}
			}

			// If here, then the container didn't have any
			// health probe matching the filtercheck
			// field.
			m_tstr = "NONE";
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	case TYPE_CONTAINER_START_TS:
		if(is_host || tinfo->m_pidns_init_start_ts == 0)
		{
			return NULL;
		}
		RETURN_EXTRACT_VAR(tinfo->m_pidns_init_start_ts);
		break;
	case TYPE_CONTAINER_DURATION:
		if(is_host || tinfo->m_clone_ts == 0)
		{
			return NULL;
		}
		m_s64val = evt->get_ts() - tinfo->m_pidns_init_start_ts;
		ASSERT(m_s64val > 0);
		RETURN_EXTRACT_VAR(m_s64val);
		break;
	case TYPE_CONTAINER_IP_ADDR:
		if(is_host)
		{
			return NULL;
		}
		else
		{
			if(!container_info)
			{
				return NULL;
			}
			m_u32val = htonl(container_info->m_container_ip);
			char addrbuff[100];
			inet_ntop(AF_INET, &m_u32val, addrbuff, sizeof(addrbuff));
			m_tstr = addrbuff;
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	case TYPE_CONTAINER_CNIRESULT:
		if(is_host)
		{
			return NULL;
		}
		else
		{
			if(!container_info)
			{
				return NULL;
			}
			RETURN_EXTRACT_STRING(container_info->m_pod_cniresult);
		}
		break;
	default:
		ASSERT(false);
		break;
	}

	return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_reference implementation
///////////////////////////////////////////////////////////////////////////////
sinsp_filter_check_reference::sinsp_filter_check_reference()
{
	m_info.m_name = "<NA>";
	m_info.m_desc = "";
	m_info.m_fields = &m_finfo;
	m_info.m_nfields = 1;
	m_info.m_flags = 0;
	m_finfo.m_print_format = PF_DEC;
	m_field = &m_finfo;
}

sinsp_filter_check* sinsp_filter_check_reference::allocate_new()
{
	ASSERT(false);
	return NULL;
}

int32_t sinsp_filter_check_reference::parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering)
{
	ASSERT(false);
	return -1;
}

uint8_t* sinsp_filter_check_reference::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = m_len;
	return m_val;
}

//
// convert a number into a byte representation.
// E.g. 1230 becomes 1.23K
//
char* sinsp_filter_check_reference::format_bytes(double val, uint32_t str_len, bool is_int)
{
	char* pr_fmt;

	if(is_int)
	{
		pr_fmt = (char*)"%*.0lf%c";
	}
	else
	{
		pr_fmt = (char*)"%*.2lf%c";
	}

	if(val > (1024LL * 1024 * 1024 * 1024 * 1024))
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					pr_fmt, str_len - 1, (val) / (1024LL * 1024 * 1024 * 1024 * 1024), 'P');
	}
	else if(val > (1024LL * 1024 * 1024 * 1024))
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					pr_fmt, str_len - 1, (val) / (1024LL * 1024 * 1024 * 1024), 'T');
	}
	else if(val > (1024LL * 1024 * 1024))
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					pr_fmt, str_len - 1, (val) / (1024LL * 1024 * 1024), 'G');
	}
	else if(val > (1024 * 1024))
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					pr_fmt, str_len - 1, (val) / (1024 * 1024), 'M');
	}
	else if(val > 1024)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					pr_fmt, str_len - 1, (val) / (1024), 'K');
	}
	else
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					pr_fmt, str_len, val, 0);
	}

	uint32_t len = (uint32_t)strlen(m_getpropertystr_storage);

	if(len > str_len)
	{
		memmove(m_getpropertystr_storage,
			m_getpropertystr_storage + len - str_len,
			str_len + 1); // include trailing \0
	}

	return m_getpropertystr_storage;
}

//
// convert a nanosecond time interval into a s.ns representation.
// E.g. 1100000000 becomes 1.1s
//
#define ONE_MILLISECOND_IN_NS 1000000
#define ONE_MICROSECOND_IN_NS 1000

char* sinsp_filter_check_reference::format_time(uint64_t val, uint32_t str_len)
{
	if(val >= 3600 * ONE_SECOND_IN_NS)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%.2u:%.2u:%.2u", (unsigned int)(val / (3600 * ONE_SECOND_IN_NS)),
					(unsigned int)((val / (60 * ONE_SECOND_IN_NS)) % 60 ),
					(unsigned int)((val / ONE_SECOND_IN_NS) % 60));
	}
	else if(val >= 60 * ONE_SECOND_IN_NS)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%u:%u", (unsigned int)(val / (60 * ONE_SECOND_IN_NS)), (unsigned int)((val / ONE_SECOND_IN_NS) % 60));
	}
	else if(val >= ONE_SECOND_IN_NS)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%u.%02us", (unsigned int)(val / ONE_SECOND_IN_NS), (unsigned int)((val % ONE_SECOND_IN_NS) / 10000000));
	}
	else if(val >= ONE_SECOND_IN_NS / 100)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%ums", (unsigned int)(val / (ONE_SECOND_IN_NS / 1000)));
	}
	else if(val >= ONE_SECOND_IN_NS / 1000)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%u.%02ums", (unsigned int)(val / (ONE_SECOND_IN_NS / 1000)), (unsigned int)((val % ONE_MILLISECOND_IN_NS) / 10000));
	}
	else if(val >= ONE_SECOND_IN_NS / 100000)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%uus", (unsigned int)(val / (ONE_SECOND_IN_NS / 1000000)));
	}
	else if(val >= ONE_SECOND_IN_NS / 1000000)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%u.%02uus", (unsigned int)(val / (ONE_SECOND_IN_NS / 1000000)), (unsigned int)((val % ONE_MICROSECOND_IN_NS) / 10));
	}
	else
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%uns", (unsigned int)val);
	}

	uint32_t reslen = (uint32_t)strlen(m_getpropertystr_storage);
	if(reslen < str_len)
	{
		uint32_t padding_size = str_len - reslen;

		memmove(m_getpropertystr_storage + padding_size,
			m_getpropertystr_storage,
			str_len + 1);

		for(uint32_t j = 0; j < padding_size; j++)
		{
			m_getpropertystr_storage[j] = ' ';
		}
	}

	return m_getpropertystr_storage;
}

char* sinsp_filter_check_reference::print_double(uint8_t* rawval, uint32_t str_len)
{
	double val;

	switch(m_field->m_type)
	{
	case PT_INT8:
		val = (double)*(int8_t*)rawval;
		break;
	case PT_INT16:
		val = (double)*(int16_t*)rawval;
		break;
	case PT_INT32:
		val = (double)*(int32_t*)rawval;
		break;
	case PT_INT64:
		val = (double)*(int64_t*)rawval;
		break;
	case PT_UINT8:
		val = (double)*(uint8_t*)rawval;
		break;
	case PT_UINT16:
		val = (double)*(uint16_t*)rawval;
		break;
	case PT_UINT32:
		val = (double)*(uint32_t*)rawval;
		break;
	case PT_UINT64:
		val = (double)*(uint64_t*)rawval;
		break;
	default:
		ASSERT(false);
		val = 0;
		break;
	}

	if(m_cnt > 1)
	{
		val /= m_cnt;
	}

	if(m_print_format == PF_ID)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%*lf", str_len, val);
		return m_getpropertystr_storage;
	}
	else
	{
		return format_bytes(val, str_len, false);
	}

}

char* sinsp_filter_check_reference::print_int(uint8_t* rawval, uint32_t str_len)
{
	int64_t val;

	switch(m_field->m_type)
	{
	case PT_INT8:
		val = (int64_t)*(int8_t*)rawval;
		break;
	case PT_INT16:
		val = (int64_t)*(int16_t*)rawval;
		break;
	case PT_INT32:
		val = (int64_t)*(int32_t*)rawval;
		break;
	case PT_INT64:
		val = (int64_t)*(int64_t*)rawval;
		break;
	case PT_UINT8:
		val = (int64_t)*(uint8_t*)rawval;
		break;
	case PT_UINT16:
		val = (int64_t)*(uint16_t*)rawval;
		break;
	case PT_UINT32:
		val = (int64_t)*(uint32_t*)rawval;
		break;
	case PT_UINT64:
		val = (int64_t)*(uint64_t*)rawval;
		break;
	default:
		ASSERT(false);
		val = 0;
		break;
	}

	if(m_cnt > 1)
	{
		val /= (int64_t)m_cnt;
	}

	if(m_print_format == PF_ID)
	{
		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%*" PRId64, str_len, val);
		return m_getpropertystr_storage;
	}
	else
	{
		return format_bytes((double)val, str_len, true);
	}

}

char* sinsp_filter_check_reference::tostring_nice(sinsp_evt* evt,
	uint32_t str_len,
	uint64_t time_delta)
{
	uint32_t len;
	// note: this uses the single-value extract because this filtercheck
	// class does not support multi-valued extraction
	uint8_t* rawval = extract(evt, &len);

	if(rawval == NULL)
	{
		return NULL;
	}

	if(time_delta != 0)
	{
		m_cnt = (double)time_delta / ONE_SECOND_IN_NS;
	}

	if(m_field->m_type >= PT_INT8 && m_field->m_type <= PT_UINT64)
	{
		if(m_print_format == PF_ID || m_cnt == 1 || m_cnt == 0)
		{
			return print_int(rawval, str_len);
		}
		else
		{
			return print_double(rawval, str_len);
		}
	}
	else if(m_field->m_type == PT_RELTIME)
	{
		double val = (double)*(uint64_t*)rawval;

		if(m_cnt > 1)
		{
			val /= m_cnt;
		}

		return format_time((int64_t)val, str_len);
	}
	else if(m_field->m_type == PT_DOUBLE)
	{
		double dval = (double)*(double*)rawval;

		if(m_cnt > 1)
		{
			dval /= m_cnt;
		}

		snprintf(m_getpropertystr_storage,
					sizeof(m_getpropertystr_storage),
					"%*.2lf", str_len, dval);
		return m_getpropertystr_storage;
	}
	else
	{
		return rawval_to_string(rawval, m_field->m_type, m_field->m_print_format, len);
	}
}

Json::Value sinsp_filter_check_reference::tojson(sinsp_evt* evt,
	uint32_t str_len,
	uint64_t time_delta)
{
	uint32_t len;
	// note: this uses the single-value extract because this filtercheck
	// class does not support multi-valued extraction
	uint8_t* rawval = extract(evt, &len);

	if(rawval == NULL)
	{
		return "";
	}

	if(time_delta != 0)
	{
		m_cnt = (double)time_delta / ONE_SECOND_IN_NS;
	}

	if(m_field->m_type == PT_RELTIME)
	{
		double val = (double)*(uint64_t*)rawval;

		if(m_cnt > 1)
		{
			val /= m_cnt;
		}

		return format_time((int64_t)val, str_len);
	}
	else if(m_field->m_type == PT_DOUBLE)
	{
		double dval = (double)*(double*)rawval;

		if(m_cnt > 1)
		{
			dval /= m_cnt;
		}

		return dval;
	}
	else
	{
		return rawval_to_json(rawval, m_field->m_type, m_field->m_print_format, len);
	}
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_utils implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_utils_fields[] =
{
	{PT_UINT64, EPF_NONE, PF_ID, "util.cnt", "Counter", "incremental counter."},
};

sinsp_filter_check_utils::sinsp_filter_check_utils()
{
	m_info.m_name = "util";
	m_info.m_desc = "";
	m_info.m_fields = sinsp_filter_check_utils_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_utils_fields) / sizeof(sinsp_filter_check_utils_fields[0]);
	m_info.m_flags = filter_check_info::FL_HIDDEN;
	m_cnt = 0;
}

sinsp_filter_check* sinsp_filter_check_utils::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_utils();
}

uint8_t* sinsp_filter_check_utils::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	switch(m_field_id)
	{
	case TYPE_CNT:
		m_cnt++;
		RETURN_EXTRACT_VAR(m_cnt);
	default:
		ASSERT(false);
		break;
	}

	return NULL;
}

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_fdlist implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_fdlist_fields[] =
{
	{PT_CHARBUF, EPF_NONE, PF_ID, "fdlist.nums", "FD Numbers", "for poll events, this is a comma-separated list of the FD numbers in the 'fds' argument, returned as a string."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fdlist.names", "FD Names", "for poll events, this is a comma-separated list of the FD names in the 'fds' argument, returned as a string."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fdlist.cips", "FD Client Addresses", "for poll events, this is a comma-separated list of the client IP addresses in the 'fds' argument, returned as a string."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "fdlist.sips", "FD Source Addresses", "for poll events, this is a comma-separated list of the server IP addresses in the 'fds' argument, returned as a string."},
	{PT_CHARBUF, EPF_NONE, PF_DEC, "fdlist.cports", "FD Client Ports", "for TCP/UDP FDs, for poll events, this is a comma-separated list of the client TCP/UDP ports in the 'fds' argument, returned as a string."},
	{PT_CHARBUF, EPF_NONE, PF_DEC, "fdlist.sports", "FD Source Ports", "for poll events, this is a comma-separated list of the server TCP/UDP ports in the 'fds' argument, returned as a string."},
};

sinsp_filter_check_fdlist::sinsp_filter_check_fdlist()
{
	m_info.m_name = "fdlist";
	m_info.m_desc = "Poll event related fields.";
	m_info.m_fields = sinsp_filter_check_fdlist_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_fdlist_fields) / sizeof(sinsp_filter_check_fdlist_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;
}

sinsp_filter_check* sinsp_filter_check_fdlist::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_fdlist();
}

uint8_t* sinsp_filter_check_fdlist::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	ASSERT(evt);
	sinsp_evt_param *parinfo;

	uint16_t etype = evt->get_type();

	if(etype == PPME_SYSCALL_POLL_E || etype == PPME_SYSCALL_PPOLL_E)
	{
		parinfo = evt->get_param(0);
	}
	else if(etype == PPME_SYSCALL_POLL_X || etype == PPME_SYSCALL_PPOLL_X)
	{
		parinfo = evt->get_param(1);
	}
	else
	{
		return NULL;
	}

	uint32_t j = 0;
	char* payload = parinfo->m_val;
	uint16_t nfds = *(uint16_t *)payload;
	uint32_t pos = 2;
	sinsp_threadinfo* tinfo = evt->get_thread_info();

	m_strval.clear();

	for(j = 0; j < nfds; j++)
	{
		bool add_comma = true;
		int64_t fd = *(int64_t *)(payload + pos);

		sinsp_fdinfo_t *fdinfo = tinfo->get_fd(fd);

		switch(m_field_id)
		{
		case TYPE_FDNUMS:
		{
			m_strval += to_string(fd);
		}
		break;
		case TYPE_FDNAMES:
		{
			if(fdinfo != NULL)
			{
				if(fdinfo->m_name != "")
				{
					m_strval += fdinfo->m_name;
				}
				else
				{
					m_strval += "<NA>";
				}
			}
			else
			{
				m_strval += "<NA>";
			}
		}
		break;
		case TYPE_CLIENTIPS:
		{
			if(fdinfo != NULL)
			{
				if(fdinfo->m_type == SCAP_FD_IPV4_SOCK)
				{
					inet_ntop(AF_INET, &fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sip, m_addrbuff, sizeof(m_addrbuff));
					m_strval += m_addrbuff;
					break;
				}
				else if(fdinfo->m_type == SCAP_FD_IPV6_SOCK)
				{
					inet_ntop(AF_INET6, fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sip.m_b, m_addrbuff, sizeof(m_addrbuff));
					m_strval += m_addrbuff;
					break;
				}
			}

			add_comma = false;
		}
		break;
		case TYPE_SERVERIPS:
		{
			if(fdinfo != NULL)
			{
				if(fdinfo->m_type == SCAP_FD_IPV4_SOCK)
				{
					inet_ntop(AF_INET, &fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dip, m_addrbuff, sizeof(m_addrbuff));
					m_strval += m_addrbuff;
					break;
				}
				else if(fdinfo->m_type == SCAP_FD_IPV6_SOCK)
				{
					inet_ntop(AF_INET6, fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dip.m_b, m_addrbuff, sizeof(m_addrbuff));
					m_strval += m_addrbuff;
					break;
				}
				else if(fdinfo->m_type == SCAP_FD_IPV4_SERVSOCK)
				{
					inet_ntop(AF_INET, &fdinfo->m_sockinfo.m_ipv4serverinfo.m_ip, m_addrbuff, sizeof(m_addrbuff));
					m_strval += m_addrbuff;
					break;
				}
				else if(fdinfo->m_type == SCAP_FD_IPV6_SERVSOCK)
				{
					inet_ntop(AF_INET, &fdinfo->m_sockinfo.m_ipv6serverinfo.m_ip.m_b, m_addrbuff, sizeof(m_addrbuff));
					m_strval += m_addrbuff;
					break;
				}
			}

			add_comma = false;
		}
		break;
		case TYPE_CLIENTPORTS:
		{
			if(fdinfo != NULL)
			{
				if(fdinfo->m_type == SCAP_FD_IPV4_SOCK)
				{
					m_strval += to_string(fdinfo->m_sockinfo.m_ipv4info.m_fields.m_sport);
					break;
				}
				else if(fdinfo->m_type == SCAP_FD_IPV6_SOCK)
				{
					m_strval += to_string(fdinfo->m_sockinfo.m_ipv6info.m_fields.m_sport);
					break;
				}
			}

			add_comma = false;
		}
		case TYPE_SERVERPORTS:
		{
			if(fdinfo != NULL)
			{
				if(fdinfo->m_type == SCAP_FD_IPV4_SOCK)
				{
					m_strval += to_string(fdinfo->m_sockinfo.m_ipv4info.m_fields.m_dport);
					break;
				}
				else if(fdinfo->m_type == SCAP_FD_IPV6_SOCK)
				{
					m_strval += to_string(fdinfo->m_sockinfo.m_ipv6info.m_fields.m_dport);
					break;
				}
			}

			add_comma = false;
		}
		break;
		default:
			ASSERT(false);
		}

		if(j < nfds && add_comma)
		{
			m_strval += ",";
		}

		pos += 10;
	}

	if(m_strval.size() != 0)
	{
		if(m_strval.back() == ',')
		{
			m_strval = m_strval.substr(0, m_strval.size() - 1);
		}

		RETURN_EXTRACT_STRING(m_strval);
	}
	else
	{
		return NULL;
	}
}

#if !defined(CYGWING_AGENT) && !defined(MINIMAL_BUILD) && !defined(__EMSCRIPTEN__)

///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_k8s implementation
///////////////////////////////////////////////////////////////////////////////
const filtercheck_field_info sinsp_filter_check_k8s_fields[] =
{
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.pod.name", "Pod Name", "Kubernetes pod name."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.pod.id", "Pod ID", "Kubernetes pod id."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "k8s.pod.label", "Pod Label", "Kubernetes pod label. E.g. 'k8s.pod.label.foo'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.pod.labels", "Pod Labels", "Kubernetes pod comma-separated key/value labels. E.g. 'foo1:bar1,foo2:bar2'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.pod.ip", "Pod Ip", "Kubernetes pod ip, same as container.ip field as each container in a pod shares the network stack of the sandbox / pod. Only ipv4 addresses are tracked. Consider k8s.pod.cni.json for logging ip addresses for each network interface."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.pod.cni.json", "Pod CNI result json", "Kubernetes pod CNI result field from the respective pod status info, same as container.cni.json field. It contains ip addresses for each network interface exposed as unparsed escaped JSON string. Supported for CRI container engine (containerd, cri-o runtimes), optimized for containerd (some non-critical JSON keys removed). Useful for tracking ips (ipv4 and ipv6, dual-stack support) for each network interface (multi-interface support)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.rc.name", "Replication Controller Name", "Kubernetes replication controller name."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.rc.id", "Replication Controller ID", "Kubernetes replication controller id."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "k8s.rc.label", "Replication Controller Label", "Kubernetes replication controller label. E.g. 'k8s.rc.label.foo'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.rc.labels", "Replication Controller Labels", "Kubernetes replication controller comma-separated key/value labels. E.g. 'foo1:bar1,foo2:bar2'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.svc.name", "Service Name", "Kubernetes service name (can return more than one value, concatenated)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.svc.id", "Service ID", "Kubernetes service id (can return more than one value, concatenated)."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "k8s.svc.label", "Service Label", "Kubernetes service label. E.g. 'k8s.svc.label.foo' (can return more than one value, concatenated)."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.svc.labels", "Service Labels", "Kubernetes service comma-separated key/value labels. E.g. 'foo1:bar1,foo2:bar2'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.ns.name", "Namespace Name", "Kubernetes namespace name."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.ns.id", "Namespace ID", "Kubernetes namespace id."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "k8s.ns.label", "Namespace Label", "Kubernetes namespace label. E.g. 'k8s.ns.label.foo'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.ns.labels", "Namespace Labels", "Kubernetes namespace comma-separated key/value labels. E.g. 'foo1:bar1,foo2:bar2'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.rs.name", "Replica Set Name", "Kubernetes replica set name."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.rs.id", "Replica Set ID", "Kubernetes replica set id."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "k8s.rs.label", "Replica Set Label", "Kubernetes replica set label. E.g. 'k8s.rs.label.foo'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.rs.labels", "Replica Set Labels", "Kubernetes replica set comma-separated key/value labels. E.g. 'foo1:bar1,foo2:bar2'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.deployment.name", "Deployment Name", "Kubernetes deployment name."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.deployment.id", "Deployment ID", "Kubernetes deployment id."},
	{PT_CHARBUF, EPF_ARG_REQUIRED, PF_NA, "k8s.deployment.label", "Deployment Label", "Kubernetes deployment label. E.g. 'k8s.rs.label.foo'."},
	{PT_CHARBUF, EPF_NONE, PF_NA, "k8s.deployment.labels", "Deployment Labels", "Kubernetes deployment comma-separated key/value labels. E.g. 'foo1:bar1,foo2:bar2'."},
};

sinsp_filter_check_k8s::sinsp_filter_check_k8s()
{
	m_info.m_name = "k8s";
	m_info.m_desc = "Kubernetes related context. When configured to fetch from the API server, all fields are available. Otherwise, only the `k8s.pod.*` and `k8s.ns.name` fields are populated with data gathered from the container runtime.";
	m_info.m_fields = sinsp_filter_check_k8s_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_k8s_fields) / sizeof(sinsp_filter_check_k8s_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;
}

sinsp_filter_check* sinsp_filter_check_k8s::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_k8s();
}

int32_t sinsp_filter_check_k8s::parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering)
{
	string val(str);

	if(STR_MATCH("k8s.pod.label") &&
		!STR_MATCH("k8s.pod.labels"))
	{
		m_field_id = TYPE_K8S_POD_LABEL;
		m_field = &m_info.m_fields[m_field_id];

		return extract_arg("k8s.pod.label", val);
	}
	else if(STR_MATCH("k8s.rc.label") &&
		!STR_MATCH("k8s.rc.labels"))
	{
		m_field_id = TYPE_K8S_RC_LABEL;
		m_field = &m_info.m_fields[m_field_id];

		return extract_arg("k8s.rc.label", val);
	}
	else if(STR_MATCH("k8s.rs.label") &&
		!STR_MATCH("k8s.rs.labels"))
	{
		m_field_id = TYPE_K8S_RS_LABEL;
		m_field = &m_info.m_fields[m_field_id];

		return extract_arg("k8s.rs.label", val);
	}
	else if(STR_MATCH("k8s.svc.label") &&
		!STR_MATCH("k8s.svc.labels"))
	{
		m_field_id = TYPE_K8S_SVC_LABEL;
		m_field = &m_info.m_fields[m_field_id];

		return extract_arg("k8s.svc.label", val);
	}
	else if(STR_MATCH("k8s.ns.label") &&
		!STR_MATCH("k8s.ns.labels"))
	{
		m_field_id = TYPE_K8S_NS_LABEL;
		m_field = &m_info.m_fields[m_field_id];

		return extract_arg("k8s.ns.label", val);
	}
	else if(STR_MATCH("k8s.deployment.label") &&
		!STR_MATCH("k8s.deployment.labels"))
	{
		m_field_id = TYPE_K8S_DEPLOYMENT_LABEL;
		m_field = &m_info.m_fields[m_field_id];

		return extract_arg("k8s.deployment.label", val);
	}
	else
	{
		return sinsp_filter_check::parse_field_name(str, alloc_state, needed_for_filtering);
	}
}

int32_t sinsp_filter_check_k8s::extract_arg(const string& fldname, const string& val)
{
	int32_t parsed_len = 0;

	if(val[fldname.size()] == '.')
	{
		size_t endpos;
		for(endpos = fldname.size() + 1; endpos < val.length(); ++endpos)
		{
			if(!isalnum(val[endpos])
				&& val[endpos] != '/'
				&& val[endpos] != '_'
				&& val[endpos] != '-'
				&& val[endpos] != '.')
			{
				break;
			}
		}

		parsed_len = (uint32_t)endpos;
		m_argname = val.substr(fldname.size() + 1, endpos - fldname.size() - 1);
	}
	else
	{
		throw sinsp_exception("filter syntax error: " + val);
	}

	return parsed_len;
}

#ifdef HAS_ANALYZER

// When using the analyzer, the necessary state is not collected, so
// these methods all return no info.

const k8s_pod_t* sinsp_filter_check_k8s::find_pod_for_thread(const sinsp_threadinfo* tinfo)
{
	return NULL;
}

const k8s_ns_t* sinsp_filter_check_k8s::find_ns_by_name(const string& ns_name)
{
	return NULL;
}

const k8s_rc_t* sinsp_filter_check_k8s::find_rc_by_pod(const k8s_pod_t* pod)
{
	return NULL;
}

const k8s_rs_t* sinsp_filter_check_k8s::find_rs_by_pod(const k8s_pod_t* pod)
{
	return NULL;
}

vector<const k8s_service_t*> sinsp_filter_check_k8s::find_svc_by_pod(const k8s_pod_t* pod)
{

	vector<const k8s_service_t *> empty;

	return empty;
}

const k8s_deployment_t* sinsp_filter_check_k8s::find_deployment_by_pod(const k8s_pod_t* pod)
{
	return NULL;
}

#else
#ifndef __EMSCRIPTEN__
const k8s_pod_t* sinsp_filter_check_k8s::find_pod_for_thread(const sinsp_threadinfo* tinfo)
{
	if(tinfo->m_container_id.empty())
	{
		return NULL;
	}

	const k8s_state_t& k8s_state = m_inspector->m_k8s_client->get_state();

	return k8s_state.get_pod(tinfo->m_container_id);
}

const k8s_ns_t* sinsp_filter_check_k8s::find_ns_by_name(const string& ns_name)
{
	const k8s_state_t& k8s_state = m_inspector->m_k8s_client->get_state();

	const k8s_state_t::namespace_map& ns_map = k8s_state.get_namespace_map();
	k8s_state_t::namespace_map::const_iterator it = ns_map.find(ns_name);
	if(it != ns_map.end())
	{
		return it->second;
	}

	return NULL;
}

const k8s_rc_t* sinsp_filter_check_k8s::find_rc_by_pod(const k8s_pod_t* pod)
{
	const k8s_state_t& k8s_state = m_inspector->m_k8s_client->get_state();

	const k8s_state_t::pod_rc_map& pod_rcs = k8s_state.get_pod_rc_map();
	k8s_state_t::pod_rc_map::const_iterator it = pod_rcs.find(pod->get_uid());
	if(it != pod_rcs.end())
	{
		return it->second;
	}

	return NULL;
}

const k8s_rs_t* sinsp_filter_check_k8s::find_rs_by_pod(const k8s_pod_t* pod)
{
	const k8s_state_t& k8s_state = m_inspector->m_k8s_client->get_state();

	const k8s_state_t::pod_rs_map& pod_rss = k8s_state.get_pod_rs_map();
	k8s_state_t::pod_rs_map::const_iterator it = pod_rss.find(pod->get_uid());
	if(it != pod_rss.end())
	{
		return it->second;
	}

	return NULL;
}

vector<const k8s_service_t*> sinsp_filter_check_k8s::find_svc_by_pod(const k8s_pod_t* pod)
{
	const k8s_state_t& k8s_state = m_inspector->m_k8s_client->get_state();
	vector<const k8s_service_t*> services;

	const k8s_state_t::pod_service_map& pod_services = k8s_state.get_pod_service_map();
	auto range = pod_services.equal_range(pod->get_uid());
	for(auto it = range.first; it != range.second; ++it)
	{
		services.push_back(it->second);
	}
	return services;
}

const k8s_deployment_t* sinsp_filter_check_k8s::find_deployment_by_pod(const k8s_pod_t* pod)
{
	const k8s_state_t& k8s_state = m_inspector->m_k8s_client->get_state();

	const k8s_state_t::pod_deployment_map& pod_deployments = k8s_state.get_pod_deployment_map();
	k8s_state_t::pod_deployment_map::const_iterator it = pod_deployments.find(pod->get_uid());
	if(it != pod_deployments.end())
	{
		return it->second;
	}

	return NULL;
}
#endif

void sinsp_filter_check_k8s::concatenate_labels(const k8s_pair_list& labels, string* s)
{
	for(const k8s_pair_t& label_pair : labels)
	{
		if(!s->empty())
		{
			s->append(", ");
		}

		s->append(label_pair.first);
		if(!label_pair.second.empty())
		{
			s->append(":" + label_pair.second);
		}
	}
}

void sinsp_filter_check_k8s::concatenate_container_labels(const map<std::string, std::string>& labels, string* s)
{
	for (auto const& label_pair : labels)
	{
		// exclude annotations and internal labels
		if (label_pair.first.find("annotation.") == 0 || label_pair.first.find("io.kubernetes.") == 0) {
			continue;
		}
		if(!s->empty())
		{
			s->append(", ");
		}
		s->append(label_pair.first);
		if(!label_pair.second.empty())
		{
			s->append(":" + label_pair.second);
		}
	}
}

bool sinsp_filter_check_k8s::find_label(const k8s_pair_list& labels, const string& key, string* value)
{
	for(const k8s_pair_t& label_pair : labels)
	{
		if(label_pair.first == key)
		{
			*value = label_pair.second;
			return true;
		}
	}

	return false;
}

uint8_t* sinsp_filter_check_k8s::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;

	ASSERT(evt);
	if(evt == NULL)
	{
		ASSERT(false);
		return NULL;
	}

	sinsp_threadinfo* tinfo = evt->get_thread_info();
	if(tinfo == NULL)
	{
		return NULL;
	}
	m_tstr.clear();
	// there is metadata we can pull from the container directly instead of the k8s apiserver
	const sinsp_container_info::ptr_t container_info =
		m_inspector->m_container_manager.get_container(tinfo->m_container_id);
	if(!tinfo->m_container_id.empty() && container_info && !container_info->m_labels.empty())
	{
		switch(m_field_id)
		{
		case TYPE_K8S_POD_NAME:
			if(container_info->m_labels.count("io.kubernetes.pod.name") > 0)
			{
				m_tstr = container_info->m_labels.at("io.kubernetes.pod.name");
				RETURN_EXTRACT_STRING(m_tstr);
			}
			break;
		case TYPE_K8S_NS_NAME:
			if(container_info->m_labels.count("io.kubernetes.pod.namespace") > 0)
			{
				m_tstr = container_info->m_labels.at("io.kubernetes.pod.namespace");
				RETURN_EXTRACT_STRING(m_tstr);
			}
			break;
		case TYPE_K8S_POD_ID:
			if(container_info->m_labels.count("io.kubernetes.pod.uid") > 0)
			{
				m_tstr = container_info->m_labels.at("io.kubernetes.pod.uid");
				RETURN_EXTRACT_STRING(m_tstr);
			}
			break;
		case TYPE_K8S_POD_LABEL:
		case TYPE_K8S_POD_LABELS:
			if(container_info->m_labels.count("io.kubernetes.sandbox.id") > 0)
			{
				std::string sandbox_container_id;
				sandbox_container_id = container_info->m_labels.at("io.kubernetes.sandbox.id");
				if(sandbox_container_id.size() > 12)
				{
					sandbox_container_id.resize(12);
				}
				const sinsp_container_info::ptr_t sandbox_container_info =
					m_inspector->m_container_manager.get_container(sandbox_container_id);
				if(sandbox_container_info && !sandbox_container_info->m_labels.empty())
				{
					if (m_field_id == TYPE_K8S_POD_LABEL && sandbox_container_info->m_labels.count(m_argname) > 0)
					{
						m_tstr = sandbox_container_info->m_labels.at(m_argname);
						RETURN_EXTRACT_STRING(m_tstr);
					}
					if (m_field_id == TYPE_K8S_POD_LABELS)
					{
						concatenate_container_labels(sandbox_container_info->m_labels, &m_tstr);
						RETURN_EXTRACT_STRING(m_tstr);
					}
				}

			}
			break;
		case TYPE_K8S_POD_IP:
			m_u32val = htonl(container_info->m_container_ip);
			char addrbuff[100];
			inet_ntop(AF_INET, &m_u32val, addrbuff, sizeof(addrbuff));
			m_tstr = addrbuff;
			RETURN_EXTRACT_STRING(m_tstr);
			break;
		case TYPE_K8S_POD_CNIRESULT:
			RETURN_EXTRACT_STRING(container_info->m_pod_cniresult);
			break;
		default:
			ASSERT(false);
			break;
		}
	}

	if(m_inspector->m_k8s_client == NULL)
	{
		return NULL;
	}

	const k8s_pod_t* pod = find_pod_for_thread(tinfo);
	if(pod == NULL)
	{
		return NULL;
	}

	switch(m_field_id)
	{
	case TYPE_K8S_POD_NAME:
		m_tstr = pod->get_name();
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_K8S_POD_ID:
		m_tstr = pod->get_uid();
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_K8S_POD_LABEL:
	{
		if(find_label(pod->get_labels(), m_argname, &m_tstr))
		{
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_POD_LABELS:
	{
		concatenate_labels(pod->get_labels(), &m_tstr);
		RETURN_EXTRACT_STRING(m_tstr);
	}
	case TYPE_K8S_RC_NAME:
	{
		const k8s_rc_t* rc = find_rc_by_pod(pod);
		if(rc != NULL)
		{
			m_tstr = rc->get_name();
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_RC_ID:
	{
		const k8s_rc_t* rc = find_rc_by_pod(pod);
		if(rc != NULL)
		{
			m_tstr = rc->get_uid();
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_RC_LABEL:
	{
		const k8s_rc_t* rc = find_rc_by_pod(pod);
		if(rc != NULL)
		{
			if(find_label(rc->get_labels(), m_argname, &m_tstr))
			{
				RETURN_EXTRACT_STRING(m_tstr);
			}
		}
		break;
	}
	case TYPE_K8S_RC_LABELS:
	{
		const k8s_rc_t* rc = find_rc_by_pod(pod);
		if(rc != NULL)
		{
			concatenate_labels(rc->get_labels(), &m_tstr);
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_RS_NAME:
	{
		const k8s_rs_t* rs = find_rs_by_pod(pod);
		if(rs != NULL)
		{
			m_tstr = rs->get_name();
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_RS_ID:
	{
		const k8s_rs_t* rs = find_rs_by_pod(pod);
		if(rs != NULL)
		{
			m_tstr = rs->get_uid();
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_RS_LABEL:
	{
		const k8s_rs_t* rs = find_rs_by_pod(pod);
		if(rs != NULL)
		{
			if(find_label(rs->get_labels(), m_argname, &m_tstr))
			{
				RETURN_EXTRACT_STRING(m_tstr);
			}
		}
		break;
	}
	case TYPE_K8S_RS_LABELS:
	{
		const k8s_rs_t* rs = find_rs_by_pod(pod);
		if(rs != NULL)
		{
			concatenate_labels(rs->get_labels(), &m_tstr);
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_SVC_NAME:
	{
		vector<const k8s_service_t*> services = find_svc_by_pod(pod);
		if(!services.empty())
		{
			for(const k8s_service_t* service : services)
			{
				if(!m_tstr.empty())
				{
					m_tstr.append(", ");
				}

				m_tstr.append(service->get_name());
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_SVC_ID:
	{
		vector<const k8s_service_t*> services = find_svc_by_pod(pod);
		if(!services.empty())
		{
			for(const k8s_service_t* service : services)
			{
				if(!m_tstr.empty())
				{
					m_tstr.append(", ");
				}

				m_tstr.append(service->get_uid());
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_SVC_LABEL:
	{
		vector<const k8s_service_t*> services = find_svc_by_pod(pod);
		if(!services.empty())
		{
			for(const k8s_service_t* service : services)
			{
				string val;
				if(find_label(service->get_labels(), m_argname, &val))
				{
					if(!m_tstr.empty())
					{
						m_tstr.append(", ");
					}

					m_tstr.append(val);
				}
			}

			if(!m_tstr.empty())
			{
				RETURN_EXTRACT_STRING(m_tstr);
			}
		}
		break;
	}
	case TYPE_K8S_SVC_LABELS:
	{
		vector<const k8s_service_t*> services = find_svc_by_pod(pod);
		if(!services.empty())
		{
			for(const k8s_service_t* service : services)
			{
				concatenate_labels(service->get_labels(), &m_tstr);
			}

			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_NS_NAME:
	{
		m_tstr = pod->get_namespace();
		RETURN_EXTRACT_STRING(m_tstr);
	}
	case TYPE_K8S_NS_ID:
	{
		const k8s_ns_t* ns = find_ns_by_name(pod->get_namespace());
		if(ns != NULL)
		{
			m_tstr = ns->get_uid();
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_NS_LABEL:
	{
		const k8s_ns_t* ns = find_ns_by_name(pod->get_namespace());
		if(ns != NULL)
		{
			if(find_label(ns->get_labels(), m_argname, &m_tstr))
			{
				RETURN_EXTRACT_STRING(m_tstr);
			}
		}
		break;
	}
	case TYPE_K8S_NS_LABELS:
	{
		const k8s_ns_t* ns = find_ns_by_name(pod->get_namespace());
		if(ns != NULL)
		{
			concatenate_labels(ns->get_labels(), &m_tstr);
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_DEPLOYMENT_NAME:
	{
		const k8s_deployment_t* deployment = find_deployment_by_pod(pod);
		if(deployment != NULL)
		{
			m_tstr = deployment->get_name();
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_DEPLOYMENT_ID:
	{
		const k8s_deployment_t* deployment = find_deployment_by_pod(pod);
		if(deployment != NULL)
		{
			m_tstr = deployment->get_uid();
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_K8S_DEPLOYMENT_LABEL:
	{
		const k8s_deployment_t* deployment = find_deployment_by_pod(pod);
		if(deployment != NULL)
		{
			if(find_label(deployment->get_labels(), m_argname, &m_tstr))
			{
				RETURN_EXTRACT_STRING(m_tstr);
			}
		}
		break;
	}
	case TYPE_K8S_DEPLOYMENT_LABELS:
	{
		const k8s_deployment_t* deployment = find_deployment_by_pod(pod);
		if(deployment != NULL)
		{
			concatenate_labels(deployment->get_labels(), &m_tstr);
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	default:
		ASSERT(false);
		return NULL;
	}

	return NULL;
}



///////////////////////////////////////////////////////////////////////////////
// sinsp_filter_check_mesos implementation
///////////////////////////////////////////////////////////////////////////////

const filtercheck_field_info sinsp_filter_check_mesos_fields[] =
{
	{PT_CHARBUF, EPF_NONE|EPF_DEPRECATED, PF_NA, "mesos.task.name", "Task Name", "Mesos task name."},
	{PT_CHARBUF, EPF_NONE|EPF_DEPRECATED, PF_NA, "mesos.task.id", "Task ID", "Mesos task id."},
	{PT_CHARBUF, EPF_ARG_REQUIRED|EPF_DEPRECATED, PF_NA, "mesos.task.label", "Task Label", "Mesos task label. E.g. 'mesos.task.label.foo'."},
	{PT_CHARBUF, EPF_NONE|EPF_DEPRECATED, PF_NA, "mesos.task.labels", "Task Labels", "Mesos task comma-separated key/value labels. E.g. 'foo1:bar1,foo2:bar2'."},
	{PT_CHARBUF, EPF_NONE|EPF_DEPRECATED, PF_NA, "mesos.framework.name", "Framework Name", "Mesos framework name."},
	{PT_CHARBUF, EPF_NONE|EPF_DEPRECATED, PF_NA, "mesos.framework.id", "Framework ID", "Mesos framework id."},
	{PT_CHARBUF, EPF_NONE|EPF_DEPRECATED, PF_NA, "marathon.app.name", "App Name", "Marathon app name."},
	{PT_CHARBUF, EPF_NONE|EPF_DEPRECATED, PF_NA, "marathon.app.id", "App ID", "Marathon app id."},
	{PT_CHARBUF, EPF_ARG_REQUIRED|EPF_DEPRECATED, PF_NA, "marathon.app.label", "App Label", "Marathon app label. E.g. 'marathon.app.label.foo'."},
	{PT_CHARBUF, EPF_NONE|EPF_DEPRECATED, PF_NA, "marathon.app.labels", "App Labels", "Marathon app comma-separated key/value labels. E.g. 'foo1:bar1,foo2:bar2'."},
	{PT_CHARBUF, EPF_NONE|EPF_DEPRECATED, PF_NA, "marathon.group.name", "Group Name", "Marathon group name."},
	{PT_CHARBUF, EPF_NONE|EPF_DEPRECATED, PF_NA, "marathon.group.id", "Group ID", "Marathon group id."},
};

sinsp_filter_check_mesos::sinsp_filter_check_mesos()
{
	m_info.m_name = "mesos";
	m_info.m_desc = "Mesos related context.";
	m_info.m_fields = sinsp_filter_check_mesos_fields;
	m_info.m_nfields = sizeof(sinsp_filter_check_mesos_fields) / sizeof(sinsp_filter_check_mesos_fields[0]);
	m_info.m_flags = filter_check_info::FL_WORKS_ON_THREAD_TABLE;
}

sinsp_filter_check* sinsp_filter_check_mesos::allocate_new()
{
	return (sinsp_filter_check*) new sinsp_filter_check_mesos();
}

int32_t sinsp_filter_check_mesos::parse_field_name(const char* str, bool alloc_state, bool needed_for_filtering)
{
	string val(str);

	if(STR_MATCH("mesos.task.label") &&
		!STR_MATCH("mesos.task.labels"))
	{
		m_field_id = TYPE_MESOS_TASK_LABEL;
		m_field = &m_info.m_fields[m_field_id];

		return extract_arg("mesos.task.label", val);
	}
	else if(STR_MATCH("marathon.app.label") &&
		!STR_MATCH("marathon.app.labels"))
	{
		m_field_id = TYPE_MARATHON_APP_LABEL;
		m_field = &m_info.m_fields[m_field_id];

		return extract_arg("marathon.app.label", val);
	}
	else
	{
		return sinsp_filter_check::parse_field_name(str, alloc_state, needed_for_filtering);
	}
}

int32_t sinsp_filter_check_mesos::extract_arg(const string& fldname, const string& val)
{
	int32_t parsed_len = 0;

	if(val[fldname.size()] == '.')
	{
		size_t endpos;
		for(endpos = fldname.size() + 1; endpos < val.length(); ++endpos)
		{
			if(!isalnum(val[endpos])
				&& val[endpos] != '/'
				&& val[endpos] != '_'
				&& val[endpos] != '-'
				&& val[endpos] != '.')
			{
				break;
			}
		}

		parsed_len = (uint32_t)endpos;
		m_argname = val.substr(fldname.size() + 1, endpos - fldname.size() - 1);
	}
	else
	{
		throw sinsp_exception("filter syntax error: " + val);
	}

	return parsed_len;
}

mesos_task::ptr_t sinsp_filter_check_mesos::find_task_for_thread(const sinsp_threadinfo* tinfo)
{
	ASSERT(m_inspector && tinfo);
	if(tinfo)
	{
		if(tinfo->m_container_id.empty())
		{
			return NULL;
		}

		if(m_inspector && m_inspector->m_mesos_client)
		{
			const sinsp_container_info::ptr_t container_info =
				m_inspector->m_container_manager.get_container(tinfo->m_container_id);
			if(!container_info || container_info->m_mesos_task_id.empty())
			{
				return NULL;
			}
			const mesos_state_t& mesos_state = m_inspector->m_mesos_client->get_state();
			return mesos_state.get_task(container_info->m_mesos_task_id);
		}
	}

	return NULL;
}

const mesos_framework* sinsp_filter_check_mesos::find_framework_by_task(mesos_task::ptr_t task)
{
	if(task && m_inspector && m_inspector->m_mesos_client)
	{
		const mesos_state_t& mesos_state = m_inspector->m_mesos_client->get_state();
		return mesos_state.get_framework_for_task(task->get_uid());
	}
	return NULL;
}

marathon_app::ptr_t sinsp_filter_check_mesos::find_app_by_task(mesos_task::ptr_t task)
{
	if(m_inspector && m_inspector->m_mesos_client)
	{
		return m_inspector->m_mesos_client->get_state().get_app(task);
	}
	return NULL;
}

marathon_group::ptr_t sinsp_filter_check_mesos::find_group_by_task(mesos_task::ptr_t task)
{
	if(m_inspector && m_inspector->m_mesos_client)
	{
		return m_inspector->m_mesos_client->get_state().get_group(task);
	}
	return NULL;
}

void sinsp_filter_check_mesos::concatenate_labels(const mesos_pair_list& labels, string* s)
{
	for(const mesos_pair_t& label_pair : labels)
	{
		if(!s->empty())
		{
			s->append(", ");
		}

		s->append(label_pair.first);
		if(!label_pair.second.empty())
		{
			s->append(":" + label_pair.second);
		}
	}
}

bool sinsp_filter_check_mesos::find_label(const mesos_pair_list& labels, const string& key, string* value)
{
	for(const mesos_pair_t& label_pair : labels)
	{
		if(label_pair.first == key)
		{
			*value = label_pair.second;
			return true;
		}
	}

	return false;
}

uint8_t* sinsp_filter_check_mesos::extract(sinsp_evt *evt, OUT uint32_t* len, bool sanitize_strings)
{
	*len = 0;
	if(!m_inspector || !m_inspector->m_mesos_client)
	{
		return NULL;
	}

	if(!evt)
	{
		ASSERT(false);
		return NULL;
	}

	sinsp_threadinfo* tinfo = evt->get_thread_info();
	if(!tinfo)
	{
		return NULL;
	}

	mesos_task::ptr_t task = find_task_for_thread(tinfo);
	if(!task)
	{
		return NULL;
	}

	m_tstr.clear();

	switch(m_field_id)
	{
	case TYPE_MESOS_TASK_NAME:
		m_tstr = task->get_name();
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_MESOS_TASK_ID:
		m_tstr = task->get_uid();
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_MESOS_TASK_LABEL:
		if(find_label(task->get_labels(), m_argname, &m_tstr))
		{
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	case TYPE_MESOS_TASK_LABELS:
		concatenate_labels(task->get_labels(), &m_tstr);
		RETURN_EXTRACT_STRING(m_tstr);
	case TYPE_MESOS_FRAMEWORK_NAME:
	{
		const mesos_framework* fw = find_framework_by_task(task);
		if(fw)
		{
			m_tstr = fw->get_name();
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_MESOS_FRAMEWORK_ID:
	{
		const mesos_framework* fw = find_framework_by_task(task);
		if(fw)
		{
			m_tstr = fw->get_uid();
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_MARATHON_APP_NAME:
	{
		marathon_app::ptr_t app = find_app_by_task(task);
		if(app != NULL)
		{
			m_tstr = app->get_name();
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_MARATHON_APP_ID:
	{
		marathon_app::ptr_t app = find_app_by_task(task);
		if(app != NULL)
		{
			m_tstr = app->get_id();
			RETURN_EXTRACT_STRING(m_tstr);
		}

		break;
	}
	case TYPE_MARATHON_APP_LABEL:
	{
		marathon_app::ptr_t app = find_app_by_task(task);
		if(app && find_label(app->get_labels(), m_argname, &m_tstr))
		{
			RETURN_EXTRACT_STRING(m_tstr);
		}

		break;
	}
	case TYPE_MARATHON_APP_LABELS:
	{
		marathon_app::ptr_t app = find_app_by_task(task);
		if(app)
		{
			concatenate_labels(app->get_labels(), &m_tstr);
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_MARATHON_GROUP_NAME:
	{
		marathon_app::ptr_t app = find_app_by_task(task);
		if(app)
		{
			m_tstr = app->get_group_id();
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	case TYPE_MARATHON_GROUP_ID:
	{
		marathon_app::ptr_t app = find_app_by_task(task);
		if(app)
		{
			m_tstr = app->get_group_id();
			RETURN_EXTRACT_STRING(m_tstr);
		}
		break;
	}
	default:
		ASSERT(false);
		return NULL;
	}

	return NULL;
}
#endif // !defined(CYGWING_AGENT) && !defined(MINIMAL_BUILD)
#endif
