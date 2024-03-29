#include <babeltrace2/babeltrace.h>
#include <ruby.h>
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include "config.h"

#define RUBY_PLUGIN_PROVIDER_LOG_TAG "RUBY-PLUGIN-PROVIDER"
#define RUBY_PLUGIN_FILE_PREFIX		"bt_plugin_"
#define RUBY_PLUGIN_FILE_PREFIX_LEN	(sizeof(RUBY_PLUGIN_FILE_PREFIX) - 1)
#define RUBY_PLUGIN_FILE_EXT		".rb"
#define RUBY_PLUGIN_FILE_EXT_LEN	(sizeof(RUBY_PLUGIN_FILE_EXT) - 1)

#define BT_ASSERT(_cond)
#define BT_LOGE_STR(_msg)
#define BT_LOGI_STR(_msg)
#define BT_LOGD_STR(_msg)
#define BT_LOGI(_fmt, ...)
#define BT_LOGE(_fmt, ...)
#define BT_LIB_LOGW(_fmt, ...)
#define BT_LIB_LOGE(_fmt, ...)
#define BT_LIB_LOGW_APPEND_CAUSE(_fmt, ...)
#define BT_LIB_LOGE_APPEND_CAUSE(_fmt, ...)
#define BT_LOG_WRITE(_lvl, _tag, _msg)
#define BT_LOG_WRITE_PRINTF(_lvl, _tag, _fmt, ...)

enum ruby_state {
	/* init_ruby() not called yet */
	RUBY_STATE_NOT_INITED,

	/* init_ruby() called once with success */
	RUBY_STATE_FULLY_INITIALIZED,

	/* init_ruby() called once without success */
	RUBY_STATE_CANNOT_INITIALIZE,

	/*
	 * init_ruby() called, but environment variable asks the
	 * Python interpreter not to be loaded.
	 */
	RUBY_STATE_WONT_INITIALIZE,
};

struct plugin_provider_ruby_data {
	enum ruby_state ruby_state;
	bool ruby_was_initialized_by_us;
};

/* Ruby symbols needed to check vm state */
extern void *ruby_current_vm_ptr;
extern int ruby_thread_has_gvl_p(void);
extern int ruby_native_thread_p(void);

static inline
int ruby_is_initialized(void) {
	return ruby_current_vm_ptr != NULL;
}

static inline
GString * g_string_append_ruby(GString *gstr, VALUE obj) {
	return g_string_append_len(gstr, RSTRING_PTR(obj), RSTRING_LEN(obj));
}

static inline
GString * format_current_exception(VALUE exception) {
	GString *exc = NULL;
	VALUE obj = Qnil;
	VALUE sep = Qnil;

	exc = g_string_new(NULL);
	if (!exc) {
		BT_LOGE("Failed to allocate a GString.");
		goto end;
	}
	obj = rb_funcall(exception, rb_intern("class"), 0);
	if (RB_TYPE_P(obj, T_CLASS)) {
		obj = rb_funcall(obj, rb_intern("to_s"), 0);
		if (RB_TYPE_P(obj, T_STRING)) {
			g_string_append_ruby(exc, obj);
			g_string_append(exc, ": ");
		}
	}
	obj = rb_funcall(exception, rb_intern("to_s"), 0);
	if (RB_TYPE_P(obj, T_STRING)) {
		g_string_append_ruby(exc, obj);
	}
	obj = rb_funcall(exception, rb_intern("backtrace"), 0);
	if (!RB_TYPE_P(obj, T_ARRAY))
		goto end;
	sep = rb_str_new_cstr("\n");
	if (!RB_TYPE_P(sep, T_STRING))
		goto end;
	obj = rb_funcall(obj, rb_intern("join"), 1, sep);
	if (!RB_TYPE_P(obj, T_STRING))
		goto end;
	if (RSTRING_LEN(obj) == 0)
		goto end;
	if (exc->len > 0) {
		g_string_append(exc, "\n");
	}
	g_string_append_ruby(exc, obj);

end:
	return exc;
}

static inline
void rberr_clear(void)
{
	if(ruby_is_initialized()) {
		rb_set_errinfo(Qnil);
	}
}

static inline
void append_ruby_traceback_error_cause(void)
{
	GString *exc = NULL;
	VALUE exception = Qnil;

	exception = rb_errinfo();
	if (exception == Qnil)
		goto end;
	exc = format_current_exception(exception);
	if (!exc) {
		BT_LOGE_STR("Failed to format Ruby exception.");
		goto end;
	}
	(void) BT_CURRENT_THREAD_ERROR_APPEND_CAUSE_FROM_UNKNOWN(
		RUBY_PLUGIN_PROVIDER_LOG_TAG, "%s", exc->str);

end:
	if (exc) {
		g_string_free(exc, TRUE);
	}
}

static inline
void log_ruby_traceback(int log_level)
{
	GString *exc = NULL;
	VALUE exception = Qnil;

	exception = rb_errinfo();
	if (exception == Qnil)
		goto end;
	exc = format_current_exception(exception);
	if (!exc) {
		BT_LOGE_STR("Failed to format Ruby exception.");
		goto end;
	}
	BT_LOG_WRITE_PRINTF(log_level, RUBY_PLUGIN_PROVIDER_LOG_TAG,
		"Exception occurred: Ruby traceback:\n%s", exc->str);

end:
	if (exc) {
		g_string_free(exc, TRUE);
	}
}

static enum bt_plugin_provider_initialize_func_status
plugin_ruby_initialize(
		bt_self_plugin_provider *self_plugin_provider)
{
	struct plugin_provider_ruby_data *data;
	enum bt_plugin_provider_initialize_func_status status =
		BT_PLUGIN_PROVIDER_INITIALIZE_FUNC_STATUS_OK;

	data = calloc(sizeof(struct plugin_provider_ruby_data), 1);

	if (!data) {
		BT_LIB_LOGE_APPEND_CAUSE(
			"Failed to allocate plugin provider data.");
		status = BT_PLUGIN_PROVIDER_INITIALIZE_FUNC_STATUS_MEMORY_ERROR;
		goto end;
	}

	data->ruby_state = RUBY_STATE_NOT_INITED;
	data->ruby_was_initialized_by_us = false;

	bt_self_plugin_provider_set_data(self_plugin_provider, data);

end:

	return status;
}

static
int init_ruby(struct plugin_provider_ruby_data *data)
{
	int ret = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_OK;
	const char *dis_ruby_env;
	VALUE obj = Qnil;
	int state;
	VALUE ruby_stack_start;

	switch (data->ruby_state) {
	case RUBY_STATE_NOT_INITED:
		break;
	case RUBY_STATE_FULLY_INITIALIZED:
		goto end;
	case RUBY_STATE_WONT_INITIALIZE:
		ret = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
		/* Ruby error cannot be accessed if not initialized */
		return ret;
	case RUBY_STATE_CANNOT_INITIALIZE:
		ret = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		/* Ruby error cannot be accessed if not initialized */
		return ret;
	default:
		ret = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		return ret;
	}

	/*
	 * User can disable Ruby plugin support with the
	 * `LIBBABELTRACE2_DISABLE_RUBY_PLUGINS` environment variable
	 * set to 1.
	 */
	dis_ruby_env = getenv("LIBBABELTRACE2_DISABLE_RUBY_PLUGINS");
	if (dis_ruby_env && strcmp(dis_ruby_env, "1") == 0) {
		BT_LOGI_STR("Ruby plugin support is disabled because the "
			"`LIBBABELTRACE2_DISABLE_RUBY_PLUGINS` environment "
			"variable is set to `1`.");
		data->ruby_state = RUBY_STATE_WONT_INITIALIZE;
		ret = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
		/* Ruby error cannot be accessed if not initialized */
		return ret;
	}
	if (!ruby_is_initialized()) {
		/* Ruby was not itialized */
		BT_LOGI_STR("Initializing Ruby VM");
		ruby_init_stack(&ruby_stack_start);
		ruby_init();
		{
			int         dummy_argc   = 2;
			const char *dummy_argv[] = {"ccs-ruby", "-e_=0"};
			ruby_options(dummy_argc, (char **)dummy_argv);
		}
		ruby_script("babeltrace2_ruby_plugin_provider");
		ruby_init_loadpath();
		data->ruby_was_initialized_by_us = true;
	} else {
		/* Ruby was initialized */
		BT_LOGI_STR("Found already initialized Ruby VM.");
		if (data->ruby_was_initialized_by_us) {
			BT_LOGE_STR(
				"Ruby VM was already initialized by us, "
				"we should not have ended here. "
				"This seems to imply reentrency "
				"during initialization.");
			ret = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
			goto end;
		}
		if (!ruby_native_thread_p()) {
			BT_LOGE_STR(
				"Not in native ruby thread, we can't call ruby "
				"or create a new native thread here.");
			ret = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
			goto end;
		}
	}

	rb_eval_string_protect("require 'rubygems'", &state);
	if (state) {
		append_ruby_traceback_error_cause();
		BT_LIB_LOGW_APPEND_CAUSE("Could not load 'rubygems'.");
		data->ruby_state = RUBY_STATE_CANNOT_INITIALIZE;
		ret = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		goto end;
	}

	rb_eval_string_protect("require 'babeltrace2'", &state);
	if (state) {
		append_ruby_traceback_error_cause();
		BT_LIB_LOGW_APPEND_CAUSE("Could not load Babeltrace 2 Ruby bindings.");
		data->ruby_state = RUBY_STATE_CANNOT_INITIALIZE;
		ret = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		goto end;
	}

	obj = rb_const_get(rb_cObject, rb_intern("RUBY_VERSION"));
	if (TYPE(obj) == T_STRING) {
		BT_LOGI("Initialized Ruby interpreter: version=\"%.*s\".",
		        (int)RSTRING_LEN(obj), RSTRING_PTR(obj));
	} else {
		BT_LOGI_STR("Initialized Ruby interpreter could not get version.");
	}
	obj = Qnil;

	/*
	 * Start garbage collection as we need to play around with the stack
	 * and want to leave clean for further calls to ruby.
	 */
	rb_eval_string_protect("GC.start", &state);
	if (state) {
		append_ruby_traceback_error_cause();
		BT_LIB_LOGW_APPEND_CAUSE("Could not run Ruby garbage collection.");
		data->ruby_state = RUBY_STATE_CANNOT_INITIALIZE;
		ret = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		goto end;
	}

	data->ruby_state = RUBY_STATE_FULLY_INITIALIZED;

end:
	log_ruby_traceback(ret == BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR ?
		BT_LOGGING_LEVEL_WARNING : BT_LOGGING_LEVEL_INFO);
	rberr_clear();
	return ret;
}

static
void plugin_ruby_finalize(
		bt_self_plugin_provider *self_plugin_provider) {
	struct plugin_provider_ruby_data *data = NULL;

	BT_ASSERT(self_plugin_provider);

	data = bt_self_plugin_provider_get_data(self_plugin_provider);

	BT_ASSERT(data);

	if (data->ruby_was_initialized_by_us) {
		RUBY_INIT_STACK;
		ruby_cleanup(0);
		BT_LOGI_STR("Finalized Ruby interpreter.");
		/* Ruby cannot be initialized after cleanup */
		data->ruby_state = RUBY_STATE_CANNOT_INITIALIZE;
	}

	g_free(data);
}

static
VALUE ruby_get_property_arr(VALUE arg) {
	void ** args = (void**)arg;
	VALUE obj = (VALUE)(args[0]);
	const char *prop_name = (const char *)(args[1]);
	ID id = rb_intern(prop_name);
	return rb_funcall(obj, id, 0);
}

static inline
int ruby_get_property(VALUE obj, const char *prop_name, VALUE *ret,
		bool fail_on_load_error) {
	int state = 0;

	uintptr_t args[2] = { (uintptr_t)obj, (uintptr_t)prop_name };
	*ret = rb_protect(ruby_get_property_arr, (VALUE)args, &state);
	if (state) {
		if (fail_on_load_error) {
			append_ruby_traceback_error_cause();
			BT_LIB_LOGW_APPEND_CAUSE(
				"Ruby plugin could not get %s property",
				prop_name);
			return BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		} else {
			BT_LIB_LOGW(
				"Ruby plugin could not get %s property",
				prop_name);
			return BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
		}
	}
	return BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_OK;
}

static
int ruby_load_plugin(VALUE rb_plugin, bool fail_on_load_error,
		struct bt_plugin **plugin_out) {
	int status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
	GString *gstr = NULL;
	VALUE rb_name = Qnil;
	VALUE rb_author = Qnil;
	VALUE rb_description = Qnil;
	VALUE rb_license = Qnil;
	VALUE rb_comp_class_addrs = Qnil;
	VALUE rb_major = Qnil;
	VALUE rb_minor = Qnil;
	VALUE rb_patch = Qnil;
	VALUE rb_version_extra = Qnil;
	unsigned int major = 0, minor = 0, patch = 0;
	const char *version_extra = NULL;

        gstr = g_string_new(NULL);
	if (!gstr) {
		BT_LOGE("Failed to allocate a GString.");
		status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_MEMORY_ERROR;
		goto error;
	}

	status = ruby_get_property(rb_plugin, "name", &rb_name,
			fail_on_load_error);
	if (status) {
		goto error;
	}
	if (!RB_TYPE_P(rb_name, T_STRING)) {
		if (fail_on_load_error) {
			BT_LIB_LOGW_APPEND_CAUSE(
				"Ruby plugin name is mandatory and should be a string");
			status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		} else {
			BT_LIB_LOGW(
				"Ruby plugin name is mandatory and should be a string");
			status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
		}
		goto error;
	}
	status = ruby_get_property(rb_plugin, "author", &rb_author,
			fail_on_load_error);
	if (status) {
		goto error;
	}
	status = ruby_get_property(rb_plugin, "description", &rb_description,
			fail_on_load_error);
	if (status) {
		goto error;
	}
	status = ruby_get_property(rb_plugin, "license", &rb_license,
			fail_on_load_error);
	if (status) {
		goto error;
	}
	status = ruby_get_property(rb_plugin, "major", &rb_major,
			fail_on_load_error);
	if (status) {
		goto error;
	}
	status = ruby_get_property(rb_plugin, "minor", &rb_minor,
			fail_on_load_error);
	if (status) {
		goto error;
	}
	status = ruby_get_property(rb_plugin, "patch", &rb_patch,
			fail_on_load_error);
	if (status) {
		goto error;
	}
	status = ruby_get_property(rb_plugin, "version_extra", &rb_version_extra,
			fail_on_load_error);
	if (status) {
		goto error;
	}
	status = ruby_get_property(rb_plugin,
			"component_class_addresses",
			&rb_comp_class_addrs, fail_on_load_error);
	if (status) {
		goto error;
	}
	if (!RB_TYPE_P(rb_comp_class_addrs, T_ARRAY)) {
		if (fail_on_load_error) {
			BT_LIB_LOGW_APPEND_CAUSE(
				"Ruby plugin component_class_addresses "
				"is mandatory and should be an array.");
			status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		} else {
			BT_LIB_LOGW(
				"Ruby plugin component_class_addresses "
				"is mandatory and should be an array.");
			status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
		}
		goto error;
	}
	if (!RARRAY_LEN(rb_comp_class_addrs)) {
		if (fail_on_load_error) {
			BT_LIB_LOGW_APPEND_CAUSE(
				"Ruby plugin component_class_addresses "
				"must not be empty.");
			status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		} else {
			BT_LIB_LOGW(
				"Ruby plugin component_class_addresses "
				"must not be empty.");
			status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
		}
		goto error;
	}
	for (long i = 0; i < RARRAY_LEN(rb_comp_class_addrs); i++) {
		VALUE num = rb_ary_entry(rb_comp_class_addrs, i);
		if ((!RB_TYPE_P(num, T_FIXNUM) &&
			!RB_TYPE_P(num, T_BIGNUM)) || !NUM2SIZET(num)) {
			if (fail_on_load_error) {
				BT_LIB_LOGW_APPEND_CAUSE(
					"Ruby plugin component class address "
					"must be a non null integer");
				status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
			} else {
				BT_LIB_LOGW(
					"Ruby plugin component class address "
					"must be a non null integer");
				status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
			}
			goto error;
		}
	}
	/* All essential entries have been validated */
	g_string_append_ruby(gstr, rb_name);
	*plugin_out = bt_plugin_create(gstr->str);
	if (!*plugin_out) {
		BT_LIB_LOGE_APPEND_CAUSE("Cannot create empty plugin object.");
		status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_MEMORY_ERROR;
		goto error;
	}
	g_string_truncate(gstr, 0);
	if (RB_TYPE_P(rb_description, T_STRING)) {
		g_string_append_ruby(gstr, rb_description);
		bt_plugin_set_description(*plugin_out, gstr->str);
		g_string_truncate(gstr, 0);
	}
	if (RB_TYPE_P(rb_author, T_STRING)) {
		g_string_append_ruby(gstr, rb_author);
		bt_plugin_set_author(*plugin_out, gstr->str);
		g_string_truncate(gstr, 0);
	}
	if (RB_TYPE_P(rb_license, T_STRING)) {
		g_string_append_ruby(gstr, rb_license);
		bt_plugin_set_license(*plugin_out, gstr->str);
		g_string_truncate(gstr, 0);
	}
	if (RB_TYPE_P(rb_major, T_FIXNUM) || RB_TYPE_P(rb_major, T_BIGNUM)) {
		major = NUM2INT(rb_major);
	}
	if (RB_TYPE_P(rb_minor, T_FIXNUM) || RB_TYPE_P(rb_minor, T_BIGNUM)) {
		minor = NUM2INT(rb_minor);
	}
	if (RB_TYPE_P(rb_patch, T_FIXNUM) || RB_TYPE_P(rb_patch, T_BIGNUM)) {
		patch = NUM2INT(rb_patch);
	}
	if (RB_TYPE_P(rb_version_extra, T_STRING)) {
		g_string_append_ruby(gstr, rb_version_extra);
		version_extra = gstr->str;
	}
	if (major != 0 || minor != 0 || patch != 0 || version_extra) {
		bt_plugin_set_version(*plugin_out, major, minor, patch, version_extra);
	}
	g_string_truncate(gstr, 0);
	for (long j = 0; j < RARRAY_LEN(rb_comp_class_addrs); j++) {
		VALUE num = rb_ary_entry(rb_comp_class_addrs, j);
		bt_component_class *comp_class = (bt_component_class *)NUM2SIZET(num);
		status = bt_plugin_add_component_class(*plugin_out,
				comp_class);
		if (status < 0) {
			BT_LIB_LOGE_APPEND_CAUSE(
				"Cannot add component class to plugin: "
				"rb-plugin-address=%p, "
				"plugin-addr=%p, plugin-name=\"%s\", "
				"comp-class-addr=%p, "
				"comp-class-name=\"%s\", "
				"comp-class-type=%s",
				rb_plugin, *plugin_out,
				bt_plugin_get_name(*plugin_out),
				comp_class,
				bt_component_class_get_name(comp_class),
				bt_component_class_type_string(
					bt_component_class_get_type(comp_class)));
			goto error;
		}
	}

	goto end;
error:
	BT_ASSERT(status != BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_OK);
end:
	if (gstr) {
		g_string_free(gstr, TRUE);
	}
	return status;
}

static
int ruby_load_file(struct plugin_provider_ruby_data *data,
		const char *path, bool fail_on_load_error,
		struct bt_plugin_set **plugin_set_out) {
        VALUE rb_plugin_array = Qnil;
	int status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
	int state;
	uint64_t plugin_count;
	GString *gstr = NULL;
	if (data->ruby_was_initialized_by_us) {
		RUBY_INIT_STACK;
	}
        gstr = g_string_new(NULL);
	if (!gstr) {
		BT_LOGE("Failed to allocate a GString.");
		status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_MEMORY_ERROR;
		goto error;
	}
	g_string_append(gstr, "BT2.load_plugin_file(\"");
	g_string_append(gstr, path);
	g_string_append(gstr, "\")");
	rb_plugin_array = rb_eval_string_protect(gstr->str, &state);
	if (state) {
		if (fail_on_load_error) {
			append_ruby_traceback_error_cause();
			BT_LIB_LOGW_APPEND_CAUSE(
				"Cannot load Ruby plugin: path=\"%s\"", path);
			status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		} else {
			BT_LIB_LOGW(
				"Cannot load Ruby plugin: path=\"%s\"", path);
			status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
		}
		goto error;
	}
	if (TYPE(rb_plugin_array) != T_ARRAY) {
		if (fail_on_load_error) {
			BT_LIB_LOGE(
				"Ruby plugin file loading failed: path=\"%s\"", path);
			status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		} else {
			BT_LIB_LOGW(
				"Ruby plugin file loading failed: path=\"%s\"", path);
			status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
		}
		goto error;
	}
	if (RARRAY_LEN(rb_plugin_array) == 0) {
		if (fail_on_load_error) {
			BT_LIB_LOGE(
				"Ruby plugin file did not register a plugin: path=\"%s\"",
				 path);
			status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		} else {
			BT_LIB_LOGW(
				"Ruby plugin file did not register a plugin: path=\"%s\"",
				 path);
			status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
		}
		goto error;
	}
	*plugin_set_out = bt_plugin_set_create();
	if (!*plugin_set_out) {
		BT_LIB_LOGE_APPEND_CAUSE("Cannot create empty plugin set.");
		status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_MEMORY_ERROR;
		goto error;
	}
	for (long i = 0; i < RARRAY_LEN(rb_plugin_array); i++) {
		struct bt_plugin *plugin = NULL;
		status = ruby_load_plugin(rb_ary_entry(rb_plugin_array, i),
			fail_on_load_error, &plugin);
		if (status) {
			if (fail_on_load_error) {
				BT_LIB_LOGE(
					"Could not load Ruby plugin %ld from "
					"file: path=\"%s\"", i, path);
			} else {
				BT_LIB_LOGW(
					"Could not load Ruby plugin %ld from "
					"file: path=\"%s\"", i, path);
			}
			BT_ASSERT(!plugin);
			goto error;
		}
		BT_ASSERT(plugin);
		BT_ASSERT(status == BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_OK);
		bt_plugin_set_path(plugin, path);
		bt_plugin_set_add_plugin(*plugin_set_out, plugin);
		bt_plugin_put_ref(plugin);
	}
	plugin_count = bt_plugin_set_get_plugin_count(*plugin_set_out);
	BT_ASSERT(plugin_count == (uint64_t)RARRAY_LEN(rb_plugin_array));
	g_string_truncate(gstr, 0);
        g_string_append_printf(gstr,
		"Created all %"PRId64" Ruby plugins from file: path=\"%s\"",
		plugin_count, path);
	for (uint64_t j = 0; j < plugin_count; j++) {
		const struct bt_plugin *plugin;
		plugin = bt_plugin_set_borrow_plugin_by_index_const(
				*plugin_set_out, j);
		BT_ASSERT(plugin);
		g_string_append_printf(gstr,
			", %"PRId64": plugin-addr=%p, plugin-name=\"%s\"",
			j, plugin, bt_plugin_get_name(plugin));
	}
	BT_LOGD_STR(gstr->str);
	status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_OK;
	goto end;
error:
	BT_ASSERT(status != BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_OK);
	log_ruby_traceback(BT_LOGGING_LEVEL_WARNING);
	rberr_clear();
	bt_plugin_set_put_ref(*plugin_set_out);
	*plugin_set_out = NULL;

end:
	if (gstr) {
		g_string_free(gstr, TRUE);
	}
	/* Cleanup the ruby stack. */
	rb_plugin_array = Qnil;
	rb_eval_string_protect("GC.start", &state);
	if (state) {
		append_ruby_traceback_error_cause();
		BT_LIB_LOGW_APPEND_CAUSE("Could not run Ruby garbage collection.");
		log_ruby_traceback(BT_LOGGING_LEVEL_WARNING);
		rberr_clear();
		bt_plugin_set_put_ref(*plugin_set_out);
		*plugin_set_out = NULL;
		return BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
	}
	return status;
}

static bt_plugin_provider_create_all_from_file_func_status
plugin_ruby_create_all_from_file(
		bt_self_plugin_provider *self_plugin_provider,
		const char *path,
		const bt_plugin_provider_create_all_from_file_options *options,
		const struct bt_plugin_set **plugin_set_out)
{
	struct plugin_provider_ruby_data *data = NULL;
	gchar *basename = NULL;
	size_t path_len;
	int status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_OK;
	bt_bool fail_on_load_error = BT_FALSE;

	BT_ASSERT(self_plugin_provider);
	BT_ASSERT(path);
	BT_ASSERT(options);

	fail_on_load_error =
		bt_plugin_provider_create_all_from_file_options_get_fail_on_load_error(
			options);

	data = bt_self_plugin_provider_get_data(self_plugin_provider);

	BT_ASSERT(data);

	if (data->ruby_state == RUBY_STATE_CANNOT_INITIALIZE) {
		BT_LIB_LOGE_APPEND_CAUSE(
			"Ruby interpreter could not be initialized previously.");
		status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		goto error;
	} else if (data->ruby_state == RUBY_STATE_WONT_INITIALIZE) {
		BT_LOGI_STR("Ruby plugin support is disabled because the "
			"`LIBBABELTRACE2_DISABLE_RUBY_PLUGINS` environment "
			"variable is set to `1`.");
		status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
		goto error;
	}

	BT_LOGI("Trying to create all Ruby plugins from file: path=\"%s\"",
		path);
	path_len = strlen(path);

	/* File name ends with `.rb` */
	if (strncmp(path + path_len - RUBY_PLUGIN_FILE_EXT_LEN,
			RUBY_PLUGIN_FILE_EXT,
			RUBY_PLUGIN_FILE_EXT_LEN) != 0) {
		BT_LOGI("Skipping non-Ruby file: path=\"%s\"", path);
		status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
		goto error;
	}

	/* File name starts with `bt_plugin_` */
	basename = g_path_get_basename(path);
	if (!basename) {
		BT_LIB_LOGE_APPEND_CAUSE(
			"Cannot get path's basename: path=\"%s\"", path);
		status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_ERROR;
		goto error;
	}

	if (strncmp(basename, RUBY_PLUGIN_FILE_PREFIX,
			RUBY_PLUGIN_FILE_PREFIX_LEN) != 0) {
		BT_LOGI("Skipping Ruby file not starting with `%s`: "
			"path=\"%s\"", RUBY_PLUGIN_FILE_PREFIX, path);
		status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
		goto error;
	}

	/*
	 * Initialize Ruby now.
	 *
	 * Similarly to Python plugin provider, we only do the
	 * initialization if needed.
	 */
	status = init_ruby(data);
	if (status) {
		/* init_ruby() logs and appends errors */
		goto error;
	}

	/* Try and load the file. */
	status = ruby_load_file(data, path, fail_on_load_error, (void *)plugin_set_out);
	if (status) {
		/* ruby_load_file() logs and appends errors */
		goto error;
	} else {
		BT_ASSERT(*plugin_set_out);
		BT_ASSERT((*plugin_set_out)->plugins->len > 0);
		goto end;
	}
	status = BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_NOT_FOUND;
error:
	BT_ASSERT(status != BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC_STATUS_OK);
end:
	return status;
}

BT_PLUGIN_PROVIDER_MODULE();

/* Initialize plug-in provider description. */
BT_PLUGIN_PROVIDER(ruby);
BT_PLUGIN_PROVIDER_DESCRIPTION("Ruby plugin support");
BT_PLUGIN_PROVIDER_AUTHOR("Brice Videau <bvideau@anl.gov>");
BT_PLUGIN_PROVIDER_LICENSE("MIT");

/* Set plug-in provider functions. */
BT_PLUGIN_PROVIDER_INITIALIZE_FUNC(plugin_ruby_initialize);
BT_PLUGIN_PROVIDER_FINALIZE_FUNC(plugin_ruby_finalize);
BT_PLUGIN_PROVIDER_CREATE_ALL_FROM_FILE_FUNC(plugin_ruby_create_all_from_file);
