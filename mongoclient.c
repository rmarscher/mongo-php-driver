/**
 *  Copyright 2009-2014 MongoDB, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef WIN32
# include <sys/types.h>
#endif

#include <php.h>
#include <zend_exceptions.h>
#include <ext/standard/php_smart_str.h>
#include <ext/standard/file.h>

#include "php_mongo.h"
#include "mongoclient.h"
#include "db.h"
#include "cursor_shared.h"
#include "bson.h"

#include "util/log.h"
#include "util/pool.h"

#include "mcon/types.h"
#include "mcon/read_preference.h"
#include "mcon/parse.h"
#include "mcon/manager.h"
#include "mcon/utils.h"


static void php_mongoclient_free(void* TSRMLS_DC);
static void stringify_server(mongo_server_def *server, smart_str *str);
static int close_connection(mongo_con_manager *manager, mongo_connection *connection);

zend_object_handlers mongo_default_handlers;
zend_object_handlers mongoclient_handlers;

ZEND_EXTERN_MODULE_GLOBALS(mongo)

zend_class_entry *mongo_ce_MongoClient;

extern zend_class_entry *mongo_ce_DB, *mongo_ce_Cursor, *mongo_ce_Exception;
extern zend_class_entry *mongo_ce_ConnectionException, *mongo_ce_Mongo, *mongo_ce_Int64;

MONGO_ARGINFO_STATIC ZEND_BEGIN_ARG_INFO_EX(arginfo___construct, 0, ZEND_RETURN_VALUE, 0)
	ZEND_ARG_INFO(0, server)
	ZEND_ARG_ARRAY_INFO(0, options, 0)
ZEND_END_ARG_INFO()

MONGO_ARGINFO_STATIC ZEND_BEGIN_ARG_INFO_EX(arginfo___get, 0, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, name)
ZEND_END_ARG_INFO()

MONGO_ARGINFO_STATIC ZEND_BEGIN_ARG_INFO_EX(arginfo_no_parameters, 0, ZEND_RETURN_VALUE, 0)
ZEND_END_ARG_INFO()

MONGO_ARGINFO_STATIC ZEND_BEGIN_ARG_INFO_EX(arginfo_selectDB, 0, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, database_name)
ZEND_END_ARG_INFO()

MONGO_ARGINFO_STATIC ZEND_BEGIN_ARG_INFO_EX(arginfo_selectCollection, 0, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, database_name)
	ZEND_ARG_INFO(0, collection_name)
ZEND_END_ARG_INFO()

MONGO_ARGINFO_STATIC ZEND_BEGIN_ARG_INFO_EX(arginfo_setReadPreference, 0, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, read_preference)
	ZEND_ARG_ARRAY_INFO(0, tags, 0)
ZEND_END_ARG_INFO()

MONGO_ARGINFO_STATIC ZEND_BEGIN_ARG_INFO_EX(arginfo_setWriteConcern, 0, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, w)
	ZEND_ARG_INFO(0, wtimeout)
ZEND_END_ARG_INFO()

MONGO_ARGINFO_STATIC ZEND_BEGIN_ARG_INFO_EX(arginfo_dropDB, 0, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, MongoDB_object_OR_database_name)
ZEND_END_ARG_INFO()

MONGO_ARGINFO_STATIC ZEND_BEGIN_ARG_INFO_EX(arginfo_killCursor, 0, ZEND_RETURN_VALUE, 1)
	ZEND_ARG_INFO(0, cursor_id)
ZEND_END_ARG_INFO()

static zend_function_entry mongo_methods[] = {
	PHP_ME(MongoClient, __construct, arginfo___construct, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, getConnections, arginfo_no_parameters, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)
	PHP_ME(MongoClient, connect, arginfo_no_parameters, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, __toString, arginfo_no_parameters, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, __get, arginfo___get, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, selectDB, arginfo_selectDB, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, selectCollection, arginfo_selectCollection, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, getReadPreference, arginfo_no_parameters, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, setReadPreference, arginfo_setReadPreference, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, getWriteConcern, arginfo_no_parameters, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, setWriteConcern, arginfo_setWriteConcern, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, dropDB, arginfo_dropDB, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, listDBs, arginfo_no_parameters, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, getHosts, arginfo_no_parameters, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, close, arginfo_no_parameters, ZEND_ACC_PUBLIC)
	PHP_ME(MongoClient, killCursor, arginfo_killCursor, ZEND_ACC_PUBLIC|ZEND_ACC_STATIC)

	PHP_FE_END
};

/* {{{ php_mongoclient_free
 */
static void php_mongoclient_free(void *object TSRMLS_DC)
{
	mongoclient *link = (mongoclient*)object;

	/* already freed */
	if (!link) {
		return;
	}

	if (link->servers) {
		mongo_servers_dtor(link->servers);
	}

	zend_object_std_dtor(&link->std TSRMLS_CC);

	efree(link);
}
/* }}} */

#if PHP_VERSION_ID >= 50400
void mongo_write_property(zval *object, zval *member, zval *value, const zend_literal *key TSRMLS_DC)
#else
void mongo_write_property(zval *object, zval *member, zval *value TSRMLS_DC)
#endif
{
	zval tmp_member;
	zend_property_info *property_info;

	if (member->type != IS_STRING) {
		tmp_member = *member;
		zval_copy_ctor(&tmp_member);
		convert_to_string(&tmp_member);
		member = &tmp_member;
	}

	property_info = zend_get_property_info(Z_OBJCE_P(object), member, 1 TSRMLS_CC);

	if (property_info && property_info->flags & ZEND_ACC_DEPRECATED) {
		php_error_docref(NULL TSRMLS_CC, MONGO_E_DEPRECATED, "The '%s' property is deprecated", Z_STRVAL_P(member));
	}
	if (property_info && property_info->flags & MONGO_ACC_READ_ONLY) {
		/* If its the object itself that is updating the property, or an
		 * inherited class, then its OK */
		if (!instanceof_function(Z_OBJCE_P(object), EG(scope) TSRMLS_CC)) {
			php_error_docref(NULL TSRMLS_CC, MONGO_E_DEPRECATED, "The '%s' property is read-only", Z_STRVAL_P(member));
			if (member == &tmp_member) {
				zval_dtor(member);
			}

			/* Disallow the property write */
			return;
		}
	}

#if PHP_VERSION_ID >= 50400
	(zend_get_std_object_handlers())->write_property(object, member, value, key TSRMLS_CC);
#else
	(zend_get_std_object_handlers())->write_property(object, member, value TSRMLS_CC);
#endif

	if (member == &tmp_member) {
		zval_dtor(member);
	}
}

#if PHP_VERSION_ID >= 50400
zval *mongo_read_property(zval *object, zval *member, int type, const zend_literal *key TSRMLS_DC)
#else
zval *mongo_read_property(zval *object, zval *member, int type TSRMLS_DC)
#endif
{
	zval *retval;
	zval tmp_member;
	zend_property_info *property_info;

	if (member->type != IS_STRING) {
		tmp_member = *member;
		zval_copy_ctor(&tmp_member);
		convert_to_string(&tmp_member);
		member = &tmp_member;
	}

	property_info = zend_get_property_info(Z_OBJCE_P(object), member, 1 TSRMLS_CC);

	if (property_info && property_info->flags & ZEND_ACC_DEPRECATED) {
		php_error_docref(NULL TSRMLS_CC, MONGO_E_DEPRECATED, "The '%s' property is deprecated", Z_STRVAL_P(member));
	}

	if (instanceof_function(Z_OBJCE_P(object), mongo_ce_MongoClient TSRMLS_CC) && strcmp(Z_STRVAL_P(member), "connected") == 0) {
		char *error_message = NULL;
		mongoclient *obj = (mongoclient *)zend_objects_get_address(object TSRMLS_CC);
		mongo_connection *conn = mongo_get_read_write_connection(obj->manager, obj->servers, MONGO_CON_FLAG_READ|MONGO_CON_FLAG_DONT_CONNECT, (char**) &error_message);

		ALLOC_INIT_ZVAL(retval);
		Z_SET_REFCOUNT_P(retval, 0);
		ZVAL_BOOL(retval, conn ? 1 : 0);
		if (error_message) {
			free(error_message);
		}
		return retval;
	}

#if PHP_VERSION_ID >= 50400
	retval = (zend_get_std_object_handlers())->read_property(object, member, type, key TSRMLS_CC);
#else
	retval = (zend_get_std_object_handlers())->read_property(object, member, type TSRMLS_CC);
#endif
	if (member == &tmp_member) {
		zval_dtor(member);
	}

	return retval;
}

#if PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION >= 3
HashTable *mongo_get_debug_info(zval *object, int *is_temp TSRMLS_DC)
{
	HashPosition pos;
	HashTable *props = zend_std_get_properties(object TSRMLS_CC);
	zval **entry;
	ulong num_key;

	zend_hash_internal_pointer_reset_ex(props, &pos);
	while (zend_hash_get_current_data_ex(props, (void **)&entry, &pos) == SUCCESS) {
		char *key;
		uint key_len;

		switch (zend_hash_get_current_key_ex(props, &key, &key_len, &num_key, 0, &pos)) {
			case HASH_KEY_IS_STRING: {
				/* Override the connected property like we do for the read_property handler */
				if (strcmp(key, "connected") == 0) {
					zval member;
					zval *tmp;
					INIT_ZVAL(member);
					ZVAL_STRINGL(&member, key, key_len, 0);

#if PHP_VERSION_ID >= 50400
					tmp = mongo_read_property(object, &member, BP_VAR_IS, NULL TSRMLS_CC);
#else
					tmp = mongo_read_property(object, &member, BP_VAR_IS TSRMLS_CC);
#endif
					convert_to_boolean_ex(entry);
					ZVAL_BOOL(*entry, Z_BVAL_P(tmp));
					/* the var is set to refcount = 0, need to set it to 1 so it'll get free()d */
					if (Z_REFCOUNT_P(tmp) == 0) {
						Z_SET_REFCOUNT_P(tmp, 1);
					}
					zval_ptr_dtor(&tmp);
				}
				break;
			}
			case HASH_KEY_IS_LONG:
			case HASH_KEY_NON_EXISTANT:
				break;
		}
		zend_hash_move_forward_ex(props, &pos);
	}

	*is_temp = 0;
	return props;
}
#endif


/* {{{ php_mongoclient_new
 */
zend_object_value php_mongoclient_new(zend_class_entry *class_type TSRMLS_DC)
{
	zend_object_value retval;
	mongoclient *intern;

	if (class_type == mongo_ce_Mongo) {
		php_error_docref(NULL TSRMLS_CC, MONGO_E_DEPRECATED, "The Mongo class is deprecated, please use the MongoClient class");
	}

	intern = (mongoclient*)emalloc(sizeof(mongoclient));
	memset(intern, 0, sizeof(mongoclient));

	zend_object_std_init(&intern->std, class_type TSRMLS_CC);
	init_properties(intern);

	retval.handle = zend_objects_store_put(intern, (zend_objects_store_dtor_t) zend_objects_destroy_object, php_mongoclient_free, NULL TSRMLS_CC);
	retval.handlers = &mongoclient_handlers;

	return retval;
}
/* }}} */

void mongo_init_MongoClient(TSRMLS_D)
{
	zend_class_entry ce;

	INIT_CLASS_ENTRY(ce, "MongoClient", mongo_methods);
	ce.create_object = php_mongoclient_new;
	mongo_ce_MongoClient = zend_register_internal_class(&ce TSRMLS_CC);

	/* make mongoclient object uncloneable, and with its own read_property */
	memcpy(&mongoclient_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
	mongoclient_handlers.clone_obj = NULL;
	mongoclient_handlers.read_property = mongo_read_property;
	mongoclient_handlers.write_property = mongo_write_property;
#if PHP_MAJOR_VERSION == 5 && PHP_MINOR_VERSION >= 3
	mongoclient_handlers.get_debug_info = mongo_get_debug_info;
#endif

	/* Mongo class constants */
	zend_declare_class_constant_string(mongo_ce_MongoClient, "DEFAULT_HOST", strlen("DEFAULT_HOST"), "localhost" TSRMLS_CC);
	zend_declare_class_constant_long(mongo_ce_MongoClient, "DEFAULT_PORT", strlen("DEFAULT_PORT"), 27017 TSRMLS_CC);
	zend_declare_class_constant_string(mongo_ce_MongoClient, "VERSION", strlen("VERSION"), PHP_MONGO_VERSION TSRMLS_CC);

	/* Read preferences types */
	zend_declare_class_constant_string(mongo_ce_MongoClient, "RP_PRIMARY", strlen("RP_PRIMARY"), "primary" TSRMLS_CC);
	zend_declare_class_constant_string(mongo_ce_MongoClient, "RP_PRIMARY_PREFERRED", strlen("RP_PRIMARY_PREFERRED"), "primaryPreferred" TSRMLS_CC);
	zend_declare_class_constant_string(mongo_ce_MongoClient, "RP_SECONDARY", strlen("RP_SECONDARY"), "secondary" TSRMLS_CC);
	zend_declare_class_constant_string(mongo_ce_MongoClient, "RP_SECONDARY_PREFERRED", strlen("RP_SECONDARY_PREFERRED"), "secondaryPreferred" TSRMLS_CC);
	zend_declare_class_constant_string(mongo_ce_MongoClient, "RP_NEAREST", strlen("RP_NEAREST"), "nearest" TSRMLS_CC);

	/* Mongo fields */
	zend_declare_property_bool(mongo_ce_MongoClient, "connected", strlen("connected"), 0, ZEND_ACC_PUBLIC|ZEND_ACC_DEPRECATED TSRMLS_CC);
	zend_declare_property_null(mongo_ce_MongoClient, "status", strlen("status"), ZEND_ACC_PUBLIC|ZEND_ACC_DEPRECATED  TSRMLS_CC);
	zend_declare_property_null(mongo_ce_MongoClient, "server", strlen("server"), ZEND_ACC_PROTECTED|ZEND_ACC_DEPRECATED  TSRMLS_CC);
	zend_declare_property_null(mongo_ce_MongoClient, "persistent", strlen("persistent"), ZEND_ACC_PROTECTED|ZEND_ACC_DEPRECATED  TSRMLS_CC);
}

/* {{{ Helper for connecting the servers */
mongo_connection *php_mongo_connect(mongoclient *link, int flags TSRMLS_DC)
{
	mongo_connection *con;
	char *error_message = NULL;

	/* We don't care about the result so although we assign it to a var, we
	 * only do that to handle errors and return it so that the calling function
	 * knows whether a connection could be obtained or not. */
	con = mongo_get_read_write_connection(link->manager, link->servers, flags, (char **) &error_message);
	if (con) {
		return con;
	}

	if (error_message) {
		zend_throw_exception(mongo_ce_ConnectionException, error_message, 71 TSRMLS_CC);
		free(error_message);
	} else {
		zend_throw_exception(mongo_ce_ConnectionException, "Unknown error obtaining connection", 72 TSRMLS_CC);
	}
	return NULL;
}
/* }}} */

/* {{{ Helper for special options, that can't be represented by a simple key
 * value pair, or options that are not actually connection string options. */
int mongo_store_option_wrapper(mongo_con_manager *manager, mongo_servers *servers, char *option_name, zval **option_value, char **error_message)
{
	/* Special cases:
	 *  - "connect" isn't supported by the URL parsing
	 *  - "readPreferenceTags" is an array of tagsets we need to iterate over
	 */
	if (strcasecmp(option_name, "connect") == 0) {
		return 4;
	}
	if (strcasecmp(option_name, "readPreferenceTags") == 0) {
		int            error = 0;
		HashPosition   pos;
		zval         **opt_entry;

		convert_to_array_ex(option_value);
		for (
			zend_hash_internal_pointer_reset_ex(Z_ARRVAL_PP(option_value), &pos);
			zend_hash_get_current_data_ex(Z_ARRVAL_PP(option_value), (void **) &opt_entry, &pos) == SUCCESS;
			zend_hash_move_forward_ex(Z_ARRVAL_PP(option_value), &pos)
		) {
			convert_to_string_ex(opt_entry);
			error = mongo_store_option(manager, servers, option_name, Z_STRVAL_PP(opt_entry), error_message);
			if (error) {
				return error;
			}
		}
		return error;
	}
	convert_to_string_ex(option_value);

	return mongo_store_option(manager, servers, option_name, Z_STRVAL_PP(option_value), error_message);
}
/* }}} */

/* {{{ proto MongoClient MongoClient->__construct([string connection_string [, array mongo_options [, array driver_options]]])
   Creates a new MongoClient object for mongo[d|s]. mongo_options are the same
   options as the connection_string, while driver_options is additional PHP
   MongoDB options, like stream context and callbacks. */
PHP_METHOD(MongoClient, __construct)
{
	php_mongo_ctor(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}

void php_mongo_ctor(INTERNAL_FUNCTION_PARAMETERS, int bc)
{
	char         *server = 0;
	int           server_len = 0;
	zend_bool     connect = 1;
	zval         *options = 0;
	zval         *slave_okay = 0;
	zval         *zdoptions = NULL;
	mongoclient  *link;
	zval        **opt_entry;
	char         *opt_key;
	int           error;
	char         *error_message = NULL;
	uint          opt_key_len;
	ulong         num_key;
	HashPosition  pos;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|s!a!/a!/", &server, &server_len, &options, &zdoptions) == FAILURE) {
		zval *object = getThis();
		ZVAL_NULL(object);
		return;
	}

	link = (mongoclient*)zend_object_store_get_object(getThis() TSRMLS_CC);

	/* Set the manager from the global manager */
	link->manager = MonGlo(manager);
	
	/* Parse the server specification
	 * Default to the mongo.default_host & mongo.default_port INI options */
	link->servers = mongo_parse_init();
	if (server_len) {
		error = mongo_parse_server_spec(link->manager, link->servers, server, (char **)&error_message);
		if (error) {
			zend_throw_exception(mongo_ce_ConnectionException, error_message, 20 + error TSRMLS_CC);
			free(error_message);
			return;
		}
	} else {
		char *tmp;

		spprintf(&tmp, 0, "%s:%ld", MonGlo(default_host), MonGlo(default_port));
		error = mongo_parse_server_spec(link->manager, link->servers, tmp, (char **)&error_message);
		efree(tmp);

		if (error) {
			zend_throw_exception(mongo_ce_ConnectionException, error_message, 0 TSRMLS_CC);
			free(error_message);
			return;
		}
	}

	/* If "w" was *not* set as an option, then assign the default */
	if (link->servers->options.default_w == -1 && link->servers->options.default_wstring == NULL) {
		if (bc) {
			/* Default to WriteConcern=0 for Mongo */
			link->servers->options.default_w = 0;
		} else {
			/* Default to WriteConcern=1 for MongoClient */
			link->servers->options.default_w = 1;
		}
	}

	/* Options through array */
	if (options) {
		for (
			zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(options), &pos);
			zend_hash_get_current_data_ex(Z_ARRVAL_P(options), (void **) &opt_entry, &pos) == SUCCESS;
			zend_hash_move_forward_ex(Z_ARRVAL_P(options), &pos)
		) {
			switch (zend_hash_get_current_key_ex(Z_ARRVAL_P(options), &opt_key, &opt_key_len, &num_key, 0, &pos)) {
				case HASH_KEY_IS_STRING: {
					int error_code = 0;

					error_code = mongo_store_option_wrapper(link->manager, link->servers, opt_key, opt_entry, (char **)&error_message);

					switch (error_code) {
						case -1: /* Deprecated options */
							if (strcasecmp(opt_key, "slaveOkay") == 0) {
								php_error_docref(NULL TSRMLS_CC, MONGO_E_DEPRECATED, "The 'slaveOkay' option is deprecated. Please switch to read-preferences");
							} else if (strcasecmp(opt_key, "timeout") == 0) {
								php_error_docref(NULL TSRMLS_CC, MONGO_E_DEPRECATED, "The 'timeout' option is deprecated. Please use 'connectTimeoutMS' instead");
							}
							break;
						case 4: /* Special options parameters, invalid for URL parsing - only possiblity is 'connect' for now */
							if (strcasecmp(opt_key, "connect") == 0) {
								convert_to_boolean_ex(opt_entry);
								connect = Z_BVAL_PP(opt_entry);
							}
							break;

						case 3: /* Logical error (i.e. conflicting options) */
						case 2: /* Unknown connection string option */
						case 1: /* Empty option name or value */
							/* Throw exception - error code is 20 + above value. They are defined in php_mongo.h */
							zend_throw_exception(mongo_ce_ConnectionException, error_message, 20 + error_code TSRMLS_CC);
							free(error_message);
							return;
					}
				} break;

				case HASH_KEY_IS_LONG:
					/* Throw exception - error code is 25. This is defined in php_mongo.h */
					zend_throw_exception(mongo_ce_ConnectionException, "Unrecognized or unsupported option", 25 TSRMLS_CC);
					return;
			}
		}
	}


	{
		int i = 0;
		zval **zcontext;
		php_stream_context *ctx = NULL;

		if (zdoptions && zend_hash_find(Z_ARRVAL_P(zdoptions), "context", strlen("context") + 1, (void**)&zcontext) == SUCCESS) {
			mongo_manager_log(link->manager, MLOG_CON, MLOG_INFO, "Found Stream context");
			ctx = php_stream_context_from_zval(*zcontext, PHP_FILE_NO_DEFAULT_CONTEXT);
		}
		link->servers->options.ctx = ctx;

		for (i = 0; i < link->servers->count; i++) {
			mongo_connection *con = mongo_manager_connection_find_by_server_definition(link->manager, link->servers->server[i]);

			if (con) {
#if PHP_VERSION_ID >= 50700
				php_stream_context_set(con->socket, ctx TSRMLS_CC);
#else
				php_stream_context_set(con->socket, ctx);
#endif
			}
		}
	}

	slave_okay = zend_read_static_property(mongo_ce_Cursor, "slaveOkay", strlen("slaveOkay"), NOISY TSRMLS_CC);
	if (Z_TYPE_P(slave_okay) != IS_NULL) {
		if (Z_BVAL_P(slave_okay)) {
			if (link->servers->read_pref.type != MONGO_RP_PRIMARY) {
				/* The server already has read preferences configured, but we're still
				 * trying to set slave okay. The spec says that's an error, so we
				 * throw an exception with code 23 (defined in php_mongo.h) */
				zend_throw_exception(mongo_ce_ConnectionException, "You can not use both slaveOkay and read-preferences. Please switch to read-preferences.", 23 TSRMLS_CC);
				return;
			} else {
				/* Old style option, that needs to be removed. For now, spec dictates
				 * it needs to be ReadPreference=SECONDARY_PREFERRED */
				link->servers->read_pref.type = MONGO_RP_SECONDARY_PREFERRED;
			}
		}
		php_error_docref(NULL TSRMLS_CC, MONGO_E_DEPRECATED, "The 'slaveOkay' option is deprecated. Please switch to read-preferences");
	}

	if (connect) {
		/* Make sure we clear any exceptions thrown if have any usable connection */
		if (php_mongo_connect(link, MONGO_CON_FLAG_READ|MONGO_CON_FLAG_DONT_FILTER TSRMLS_CC)) {
			zend_clear_exception(TSRMLS_C);
		}
	}
}
/* }}} */


/* {{{ proto bool MongoClient->connect(void)
   Runs topology discovery, establishes connections to all MongoDBs if not connected already */
PHP_METHOD(MongoClient, connect)
{
	mongoclient *link;

	PHP_MONGO_GET_LINK(getThis());
	RETURN_BOOL(php_mongo_connect(link, MONGO_CON_FLAG_READ TSRMLS_CC) != NULL);
}
/* }}} */


/* {{{ proto int MongoClient->close([string|bool hash|all])
   Closes the connection to $hash, or only master - or all open connections. Returns how many connections were closed */
PHP_METHOD(MongoClient, close)
{
	char             *hash = NULL;
	int               hash_len;
	mongoclient       *link;
	mongo_connection *connection;
	char             *error_message = NULL;
	zval             *all = NULL;

	PHP_MONGO_GET_LINK(getThis());
	if (ZEND_NUM_ARGS() == 0) {
		/* BC: Close master when no arguments passed */
		connection = mongo_get_read_write_connection(link->manager, link->servers, MONGO_CON_FLAG_WRITE|MONGO_CON_FLAG_DONT_CONNECT, (char **) &error_message);
		RETVAL_LONG(close_connection(link->manager, connection));
	} else if (zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS() TSRMLS_CC, "z", &all) == SUCCESS && Z_TYPE_P(all) == IS_BOOL) {
		if (Z_BVAL_P(all)) {
			/* Close all connections */
			mongo_con_manager_item *ptr = link->manager->connections;
			mongo_con_manager_item *current;
			long                    count = 0;

			while (ptr) {
				current = ptr;
				ptr = ptr->next;
				close_connection(link->manager, (mongo_connection *)current->data);
				count++;
			}

			RETVAL_LONG(count);
		} else {
			/* Close master */
			connection = mongo_get_read_write_connection(link->manager, link->servers, MONGO_CON_FLAG_WRITE|MONGO_CON_FLAG_DONT_CONNECT, (char **) &error_message);
			RETVAL_LONG(close_connection(link->manager, connection));
		}
	} else if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &hash, &hash_len) == SUCCESS) {
		/* Lookup hash and destroy it */
		connection = mongo_manager_connection_find_by_hash(link->manager, hash);
		if (!connection) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "A connection with hash '%s' does not exist.", hash);
			RETURN_LONG(0);
		}
		RETVAL_LONG(close_connection(link->manager, connection));
	} else {
		return;
	}

	if (error_message) {
		free(error_message);
	}
	RETURN_TRUE;
}
/* }}} */

static int close_connection(mongo_con_manager *manager, mongo_connection *connection)
{
	if (connection) {
		mongo_manager_connection_deregister(manager, connection);
		return 1;
	} else {
		return 0;
	}
}

static void stringify_server(mongo_server_def *server, smart_str *str)
{
	/* copy host */
	smart_str_appends(str, server->host);
	smart_str_appendc(str, ':');
	smart_str_append_long(str, server->port);
}


/* {{{ proto string MongoClient->__toString(void)
   Returns comma seperated list of servers we try to use */
PHP_METHOD(MongoClient, __toString)
{
	smart_str str = { NULL, 0, 0 };
	mongoclient *link;
	int i;

	PHP_MONGO_GET_LINK(getThis());

	for (i = 0; i < link->servers->count; i++) {
		/* if this is not the first one, add a comma */
		if (i) {
			smart_str_appendc(&str, ',');
		}

		stringify_server(link->servers->server[i], &str);
	}

	smart_str_0(&str);
	RETURN_STRINGL(str.c, str.len, 0);
}
/* }}} */

/* Selects a database and returns it as zval. If the return value is NULL, an
 * exception is set. This should only happen if the passed client is invalid or
 * the database name is invalid. */
zval *php_mongoclient_selectdb(zval *zlink, char *name, int name_len TSRMLS_DC)
{
	zval *zdb;
	mongoclient *link;
	int free_zlink_ptr = 0;

	if (memchr(name, '\0', name_len) != NULL) {
		zend_throw_exception_ex(mongo_ce_Exception, 2 TSRMLS_CC, "'\\0' not allowed in database names: %s\\0...", name);
		return NULL;
	}

	link = (mongoclient*) zend_object_store_get_object(zlink TSRMLS_CC);

	if (link == NULL || link->servers == NULL) {
		zend_throw_exception(mongo_ce_Exception, "The MongoClient object has not been correctly initialized by its constructor", 0 TSRMLS_CC);
		return NULL;
	}

	/* We need to check whether we are switching to a database that was not
	 * part of the connection string. This is not a problem if we are not using
	 * authentication, but it is if we are. If we are, we need to do some fancy
	 * cloning and creating a new mongo_servers structure. Authentication is a
	 * pain™ */
	if (link->servers->server[0]->db && strcmp(link->servers->server[0]->db, name) != 0) {
		mongo_manager_log(
			link->manager, MLOG_CON, MLOG_INFO,
			"The requested database (%s) is not what we have in the link info (%s)",
			name, link->servers->server[0]->db
		);
		/* So here we check if a username and password are used. If so, the
		 * madness starts */
		if (link->servers->server[0]->username && link->servers->server[0]->password) {
			zval *zlink_tmp;
			mongoclient *new_link;
			int i;

			if (strcmp(link->servers->server[0]->db, "admin") == 0) {
				mongo_manager_log(
					link->manager, MLOG_CON, MLOG_FINE,
					"The link info has 'admin' as database, no need to clone it then"
				);
			} else {
				mongo_manager_log(
					link->manager, MLOG_CON, MLOG_INFO,
					"We are in an authenticated link (db: %s, user: %s), so we need to clone it.",
					link->servers->server[0]->db, link->servers->server[0]->username
				);

				/* Create the new link object */
				MAKE_STD_ZVAL(zlink_tmp);
				object_init_ex(zlink_tmp, mongo_ce_MongoClient);
				new_link = (mongoclient*) zend_object_store_get_object(zlink_tmp TSRMLS_CC);

				new_link->manager = link->manager;
				new_link->servers = calloc(1, sizeof(mongo_servers));
				mongo_servers_copy(new_link->servers, link->servers, MONGO_SERVER_COPY_CREDENTIALS);
				/* We assume the previous credentials will work on this
				 * database too, or if authSource is set, authenticate against
				 * that database */
				for (i = 0; i < new_link->servers->count; i++) {
					new_link->servers->server[i]->db = strdup(name);
				}

				zlink = zlink_tmp;
				free_zlink_ptr = 1;
			}
		}
	}

	MAKE_STD_ZVAL(zdb);
	object_init_ex(zdb, mongo_ce_DB);

	if (FAILURE == php_mongodb_init(zdb, zlink, name, name_len TSRMLS_CC)) {
		zval_ptr_dtor(&zdb);
		zdb = NULL;
	}

	if (free_zlink_ptr) {
		zval_ptr_dtor(&zlink);
	}

	return zdb;
}

/* {{{ proto MongoDB MongoClient->selectDB(string dbname)
   Returns a new MongoDB object for the specified database name */
PHP_METHOD(MongoClient, selectDB)
{
	char *name;
	int   name_len;
	zval *db;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &name, &name_len) == FAILURE) {
		return;
	}

	db = php_mongoclient_selectdb(getThis(), name, name_len TSRMLS_CC);

	if (db != NULL) {
		RETURN_ZVAL(db, 0, 1);
	}
}
/* }}} */


/* {{{ proto MongoDB MongoClient::__get(string dbname)
   Returns a new MongoDB object for the specified database name */
PHP_METHOD(MongoClient, __get)
{
	char *name;
	int   name_len;
	zval *db;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &name, &name_len) == FAILURE) {
		return;
	}

	db = php_mongoclient_selectdb(getThis(), name, name_len TSRMLS_CC);

	if (db != NULL) {
		RETURN_ZVAL(db, 0, 1);
	}
}
/* }}} */


/* {{{ proto MongoCollection MongoClient::selectCollection(string dbname, string collection_name)
   Returns a new MongoCollection for the specified database and collection names */
PHP_METHOD(MongoClient, selectCollection)
{
	char *db, *coll;
	int db_len, coll_len;
	zval *z_db, *z_collection;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss", &db, &db_len, &coll, &coll_len) == FAILURE) {
		return;
	}

	z_db = php_mongoclient_selectdb(getThis(), db, db_len TSRMLS_CC);

	if (z_db == NULL) {
		return;
	}

	z_collection = php_mongo_selectcollection(z_db, coll, coll_len TSRMLS_CC);

	if (z_collection != NULL) {
		/* Only copy the zval into return_value if it worked. If collection is
		 * NULL here, an exception is set */
		RETVAL_ZVAL(z_collection, 0, 1);
	}

	zval_ptr_dtor(&z_db);
}
/* }}} */


/* {{{ proto array MongoClient::getReadPreference(void)
   Returns the currently set read preference.*/
PHP_METHOD(MongoClient, getReadPreference)
{
	mongoclient *link;
	PHP_MONGO_GET_LINK(getThis());

	array_init(return_value);
	add_assoc_string(return_value, "type", mongo_read_preference_type_to_name(link->servers->read_pref.type), 1);
	php_mongo_add_tagsets(return_value, &link->servers->read_pref);
}
/* }}} */


/* {{{ proto bool MongoClient::setReadPreference(string read_preference [, array tags ])
   Sets a read preference to be used for all read queries.*/
PHP_METHOD(MongoClient, setReadPreference)
{
	char *read_preference;
	int   read_preference_len;
	mongoclient *link;
	HashTable  *tags = NULL;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|h", &read_preference, &read_preference_len, &tags) == FAILURE) {
		return;
	}

	PHP_MONGO_GET_LINK(getThis());

	if (php_mongo_set_readpreference(&link->servers->read_pref, read_preference, tags TSRMLS_CC)) {
		RETURN_TRUE;
	} else {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ array MongoClient::getWriteConcern()
 * Get the MongoClient write concern, which will be inherited by constructed
 * MongoDB and MongoCollection objects. */
PHP_METHOD(MongoClient, getWriteConcern)
{
	mongoclient *link;

	if (zend_parse_parameters_none()) {
		return;
	}

	PHP_MONGO_GET_LINK(getThis());

	array_init(return_value);

	if (link->servers->options.default_wstring != NULL) {
		add_assoc_string(return_value, "w", link->servers->options.default_wstring, 1);
	} else {
		add_assoc_long(return_value, "w", link->servers->options.default_w);
	}

	add_assoc_long(return_value, "wtimeout", link->servers->options.default_wtimeout);
}
/* }}} */

/* {{{ bool MongoClient::setWriteConcern(mixed w [, int wtimeout])
 * Set the MongoClient write concern, which will be inherited by constructed
 * MongoDB and MongoCollection objects. */
PHP_METHOD(MongoClient, setWriteConcern)
{
	mongoclient *link;
	zval *write_concern;
	long wtimeout;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|l", &write_concern, &wtimeout) == FAILURE) {
		return;
	}

	if (Z_TYPE_P(write_concern) != IS_LONG && Z_TYPE_P(write_concern) != IS_STRING) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "expects parameter 1 to be an string or integer, %s given", zend_get_type_by_const(Z_TYPE_P(write_concern)));
		RETURN_FALSE;
	}

	PHP_MONGO_GET_LINK(getThis());

	if (link->servers->options.default_wstring) {
		free(link->servers->options.default_wstring);
	}

	if (Z_TYPE_P(write_concern) == IS_LONG) {
		link->servers->options.default_w = Z_LVAL_P(write_concern);
		link->servers->options.default_wstring = NULL;
	} else if (Z_TYPE_P(write_concern) == IS_STRING) {
		link->servers->options.default_w = 1;
		link->servers->options.default_wstring = strdup(Z_STRVAL_P(write_concern));
	}

	if (ZEND_NUM_ARGS() > 1) {
		link->servers->options.default_wtimeout = wtimeout;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto array MongoClient::dropDB(void)
   Returns the results of the 'dropDatabase' command */
PHP_METHOD(MongoClient, dropDB)
{
	zval *db;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &db) == FAILURE) {
		RETURN_FALSE;
	}

	if (Z_TYPE_P(db) != IS_OBJECT || Z_OBJCE_P(db) != mongo_ce_DB) {
		convert_to_string_ex(&db);

		db = php_mongoclient_selectdb(getThis(), Z_STRVAL_P(db), Z_STRLEN_P(db) TSRMLS_CC);

		if (db == NULL) {
			return;
		}
	} else {
		zval_add_ref(&db);
	}

	MONGO_METHOD(MongoDB, drop, return_value, db);
	zval_ptr_dtor(&db);
}
/* }}} */

/* {{{ proto array MongoClient->listDBs(void)
   Returns the results of the 'listDatabases' command, executed on the 'admin' database */
PHP_METHOD(MongoClient, listDBs)
{
	zval *cmd, *zdb, *retval;
	mongo_db *db;

	zdb = php_mongoclient_selectdb(getThis(), "admin", 5 TSRMLS_CC);

	if (zdb == NULL) {
		return;
	}

	PHP_MONGO_GET_DB(zdb);

	MAKE_STD_ZVAL(cmd);
	array_init(cmd);
	add_assoc_long(cmd, "listDatabases", 1);

	retval = php_mongo_runcommand(db->link, &db->read_pref, Z_STRVAL_P(db->name), Z_STRLEN_P(db->name), cmd, NULL, 0, NULL TSRMLS_CC);

	zval_ptr_dtor(&cmd);
	zval_ptr_dtor(&zdb);

	if (retval) {
		RETVAL_ZVAL(retval, 0, 1);
	}
}
/* }}} */

/* {{{ proto array MongoClient->getHosts(void)
   Returns an array per connection with connection info */
PHP_METHOD(MongoClient, getHosts)
{
	mongoclient            *link;
	mongo_con_manager_item *item;

	PHP_MONGO_GET_LINK(getThis());
	item = link->manager->connections;

	array_init(return_value);

	while (item) {
		zval *infoz;
		char *host;
		int   port;
		mongo_connection *con = (mongo_connection*) item->data;

		MAKE_STD_ZVAL(infoz);
		array_init(infoz);

		mongo_server_split_hash(con->hash, (char**) &host, (int*) &port, NULL, NULL, NULL, NULL, NULL);
		add_assoc_string(infoz, "host", host, 1);
		add_assoc_long(infoz, "port", port);
		free(host);

		add_assoc_long(infoz, "health", 1);
		add_assoc_long(infoz, "state", con->connection_type == MONGO_NODE_PRIMARY ? 1 : (con->connection_type == MONGO_NODE_SECONDARY ? 2 : 0));
		add_assoc_long(infoz, "ping", con->ping_ms);
		add_assoc_long(infoz, "lastPing", con->last_ping);

		add_assoc_zval(return_value, con->hash, infoz);
		item = item->next;
	}
}
/* }}} */

/* {{{ proto static array MongoClient::getConnections(void)
   Returns an array of all open connections, and information about each of the servers */
PHP_METHOD(MongoClient, getConnections)
{
	mongo_con_manager_item *ptr;

	if (zend_parse_parameters_none() == FAILURE) {
		return;
	}

	ptr = MonGlo(manager)->connections;

	array_init(return_value);
	while (ptr) {
		zval *entry, *server, *connection, *tags, *version;
		char *host, *repl_set_name, *database, *username, *auth_hash;
		int port, pid, i;
		mongo_connection *con = (mongo_connection*) ptr->data;

		MAKE_STD_ZVAL(entry);
		array_init(entry);

		MAKE_STD_ZVAL(server);
		array_init(server);

		MAKE_STD_ZVAL(connection);
		array_init(connection);

		MAKE_STD_ZVAL(tags);
		array_init(tags);

		/* Grab server information */
		mongo_server_split_hash(con->hash, &host, &port, &repl_set_name, &database, &username, &auth_hash, &pid);

		add_assoc_string(server, "host", host, 1);
		free(host);
		add_assoc_long(server, "port", port);
		if (repl_set_name) {
			add_assoc_string(server, "repl_set_name", repl_set_name, 1);
			free(repl_set_name);
		}
		if (database) {
			add_assoc_string(server, "database", database, 1);
			free(database);
		}
		if (username) {
			add_assoc_string(server, "username", username, 1);
			free(username);
		}
		if (auth_hash) {
			add_assoc_string(server, "auth_hash", auth_hash, 1);
			free(auth_hash);
		}
		add_assoc_long(server, "pid", pid);
		
		/* Add server version as array */
		MAKE_STD_ZVAL(version);
		array_init(version);
		add_assoc_long(version, "major", con->version.major);
		add_assoc_long(version, "minor", con->version.minor);
		add_assoc_long(version, "mini",  con->version.mini);
		add_assoc_long(version, "build", con->version.build);
		add_assoc_zval(server, "version", version);

		/* Add [min|max]WireVersion */
		add_assoc_long(connection, "min_wire_version", con->min_wire_version);
		add_assoc_long(connection, "max_wire_version", con->max_wire_version);

		/* Add wire protocol and command limits */
		add_assoc_long(connection, "max_bson_size", con->max_bson_size);
		add_assoc_long(connection, "max_message_size", con->max_message_size);
		add_assoc_long(connection, "max_write_batch_size", con->max_write_batch_size);

		/* Grab connection info */
		add_assoc_long(connection, "last_ping", con->last_ping);
		add_assoc_long(connection, "last_ismaster", con->last_ismaster);
		add_assoc_long(connection, "ping_ms", con->ping_ms);
		add_assoc_long(connection, "connection_type", con->connection_type);
		add_assoc_string(connection, "connection_type_desc", mongo_connection_type(con->connection_type), 1);
		add_assoc_long(connection, "tag_count", con->tag_count);
		for (i = 0; i < con->tag_count; i++) {
			add_next_index_string(tags, con->tags[i], 1);
		}
		add_assoc_zval(connection, "tags", tags);

		/* Top level elements */
		add_assoc_string(entry, "hash", con->hash, 1);
		add_assoc_zval(entry, "server", server);
		add_assoc_zval(entry, "connection", connection);
		add_next_index_zval(return_value, entry);

		ptr = ptr->next;
	}
}
/* }}} */

/* {{{ proto static bool MongoClient::killCursor(string server_hash, int|MongoInt64 id)
   Attempts to kill the cursor on the server with the specified ID, returns whether it was tried or not */
PHP_METHOD(MongoClient, killCursor)
{
	char *hash;
	int   hash_len;
	long cursor_id = 0;
	mongo_connection *con;
	zval *int64_id = NULL;

	if (zend_parse_parameters_ex(ZEND_PARSE_PARAMS_QUIET, ZEND_NUM_ARGS() TSRMLS_CC, "sO", &hash, &hash_len, &int64_id, mongo_ce_Int64) == FAILURE) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sl", &hash, &hash_len, &cursor_id) == FAILURE) {
			return;
		}
	}

	con = mongo_manager_connection_find_by_hash(MonGlo(manager), hash);

	if (!con) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "A connection with hash '%s' does not exist", hash);
		RETURN_FALSE;
	}

	if (int64_id) {
		zval *z_int64 = zend_read_property(mongo_ce_Int64, int64_id, "value", strlen("value"), NOISY TSRMLS_CC);

#ifndef WIN32
		php_mongo_kill_cursor(con, atoll(Z_STRVAL_P(z_int64)) TSRMLS_CC);
#else
		php_mongo_kill_cursor(con, _atoi64(Z_STRVAL_P(z_int64)) TSRMLS_CC);
#endif
	} else {
		php_mongo_kill_cursor(con, cursor_id TSRMLS_CC);
	}
	RETURN_TRUE;
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: fdm=marker
 * vim: noet sw=4 ts=4
 */
