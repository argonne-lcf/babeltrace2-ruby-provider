lib_LTLIBRARIES = babeltrace2_ruby_plugin_provider.la

babeltrace2_ruby_plugin_provider_la_SOURCES = \
	ruby-plugin-provider.c

babeltrace2_ruby_plugin_provider_la_LDFLAGS = \
	$(AM_LDFLAGS) \
	$(LT_NO_UNDEFINED) \
	-avoid-version -module \
	$(BABELTRACE2_LIBS) \
	$(GLIB_LIBS) \
	$(RUBY_LIBS)

babeltrace2_ruby_plugin_provider_la_CFLAGS = \
	$(AM_CFLAGS) \
	$(BABELTRACE2_CFLAGS) \
	$(GLIB_CFLAGS) \
	$(RUBY_CFLAGS)
