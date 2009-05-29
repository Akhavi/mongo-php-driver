//collection.c
/**
 *  Copyright 2009 10gen, Inc.
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

#include <php.h>
#include <zend_exceptions.h>
#include "ext/spl/spl_exceptions.h"

#include "mongo.h"
#include "collection.h"
#include "cursor.h"
#include "bson.h"
#include "mongo_types.h"
#include "db.h"

extern zend_class_entry *mongo_ce_Mongo,
  *mongo_ce_DB,
  *mongo_ce_Cursor,
  *mongo_ce_Exception;

extern int le_pconnection,
  le_connection;

extern zend_object_handlers mongo_default_handlers;

ZEND_EXTERN_MODULE_GLOBALS(mongo);

zend_class_entry *mongo_ce_Collection = NULL;

PHP_METHOD(MongoCollection, __construct) {
  zval *db, *name, *zns;
  mongo_collection *c;
  char *ns;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Oz", &db, mongo_ce_DB, &name) == FAILURE) {
    return;
  }
  convert_to_string(name);

  if (strchr(Z_STRVAL_P(name), '$')) {
    zend_throw_exception(spl_ce_InvalidArgumentException, "MongoCollection::__construct(): collection names cannot contain '$'", 0 TSRMLS_CC);
    return;
  }
  zend_update_property(mongo_ce_Collection, getThis(), "db", strlen("db"), db TSRMLS_CC);

  c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  c->parent = db;
  zval_add_ref(&c->parent);

  c->db = (mongo_db*)zend_object_store_get_object(db TSRMLS_CC);

  c->name = name;
  zval_add_ref(&name);

  spprintf(&ns, 0, "%s.%s", Z_STRVAL_P(c->db->name), Z_STRVAL_P(name));

  MAKE_STD_ZVAL(zns);
  ZVAL_STRING(zns, ns, 0);
  c->ns = zns;
}

PHP_METHOD(MongoCollection, __toString) {
  mongo_collection *c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);
  RETURN_ZVAL(c->ns, 1, 0);
}

PHP_METHOD(MongoCollection, getName) {
  mongo_collection *c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);
  RETURN_ZVAL(c->name, 1, 0);
}

PHP_METHOD(MongoCollection, drop) {
  zval *data;
  mongo_collection *c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  MAKE_STD_ZVAL(data);
  array_init(data);
  add_assoc_string(data, "drop", Z_STRVAL_P(c->name), 1);

  PUSH_PARAM(data); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoDB_command(1, return_value, return_value_ptr, c->parent, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM();

  zval_ptr_dtor(&data);
}

PHP_METHOD(MongoCollection, validate) {
  zval *data;
  zend_bool scan_data = 0;
  mongo_collection *c;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|b", &scan_data) == FAILURE) {
    return;
  }

  c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  MAKE_STD_ZVAL(data);
  array_init(data);
  add_assoc_string(data, "validate", Z_STRVAL_P(c->name), 1);
  add_assoc_bool(data, "scandata", scan_data);

  PUSH_PARAM(data); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoDB_command(1, return_value, return_value_ptr, c->parent, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM();

  zval_ptr_dtor(&data);
}

PHP_METHOD(MongoCollection, insert) {
  zval *a;
  mongo_collection *c;
  mongo_link *link;
  int response;
  mongo_msg_header header;
  CREATE_BUF(buf, INITIAL_BUF_SIZE);

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a", &a) == FAILURE) {
    efree(buf.start);
    return;
  }

  c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  ZEND_FETCH_RESOURCE2(link, mongo_link*, &c->db->link, -1, PHP_CONNECTION_RES_NAME, le_connection, le_pconnection); 

  CREATE_HEADER(buf, Z_STRVAL_P(c->ns), Z_STRLEN_P(c->ns), OP_INSERT);

  // serialize
  if (zval_to_bson(&buf, a, PREP TSRMLS_CC) == 0) {
    efree(buf.start);
    // return if there were 0 elements
    RETURN_FALSE;
  }

  serialize_size(buf.start, &buf);

  // sends
  response = mongo_say(link, &buf TSRMLS_CC);
  efree(buf.start);
  
  RETURN_BOOL(response >= SUCCESS);
}

PHP_METHOD(MongoCollection, batchInsert) {
  zval *a;
  mongo_collection *c;
  mongo_link *link;
  HashTable *php_array;
  int count = 0, start = 0;
  zval **data;
  HashPosition pointer;
  mongo_msg_header header;
  CREATE_BUF(buf, INITIAL_BUF_SIZE);

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a", &a) == FAILURE) {
    return;
  }

  c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  ZEND_FETCH_RESOURCE2(link, mongo_link*, &c->db->link, -1, PHP_CONNECTION_RES_NAME, le_connection, le_pconnection); 

  CREATE_HEADER(buf, Z_STRVAL_P(c->ns), Z_STRLEN_P(c->ns), OP_INSERT);

  php_array = Z_ARRVAL_P(a);

  for(zend_hash_internal_pointer_reset_ex(php_array, &pointer); 
      zend_hash_get_current_data_ex(php_array, (void**) &data, &pointer) == SUCCESS; 
      zend_hash_move_forward_ex(php_array, &pointer)) {

    if(Z_TYPE_PP(data) != IS_ARRAY) {
      continue;
    }

    start = buf.pos-buf.start;
    zval_to_bson(&buf, *data, NO_PREP TSRMLS_CC);

    serialize_size(buf.start+start, &buf);

    count++;
  }

  // if there are no elements, don't bother saving
  if (count == 0) {
    efree(buf.start);
    RETURN_FALSE;
  }

  serialize_size(buf.start, &buf);

  RETVAL_BOOL(mongo_say(link, &buf TSRMLS_CC)+1);
  efree(buf.start);
}

PHP_METHOD(MongoCollection, find) {
  zval *query = 0, *fields = 0;
  mongo_collection *c;
  zval temp;
  int i = 0;
  
  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|aa", &query, &fields) == FAILURE) {
    return;
  }

  c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  object_init_ex(return_value, mongo_ce_Cursor);

  PUSH_PARAM(c->db->link); PUSH_PARAM(c->ns); 
  if (query) {
    PUSH_PARAM(query);
    if (fields) {
      PUSH_PARAM(fields);
    }
  }
  
  PUSH_PARAM((void*)(ZEND_NUM_ARGS()+2));
  PUSH_EO_PARAM();

  zim_MongoCursor___construct(ZEND_NUM_ARGS()+2, &temp, NULL, return_value, return_value_used TSRMLS_CC);

  POP_EO_PARAM();
  while(i++ < ZEND_NUM_ARGS()+3) {
    POP_PARAM();
  }
}

PHP_METHOD(MongoCollection, findOne) {
  zval *query = 0, *cursor;
  zval limit;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|a", &query) == FAILURE) {
    return;
  }

  MAKE_STD_ZVAL(cursor);

  if (query) {
    PUSH_PARAM(query);
    PUSH_PARAM((void*)1);
    PUSH_EO_PARAM();
  }
  zim_MongoCollection_find(ZEND_NUM_ARGS(), cursor, &cursor, getThis(), return_value_used TSRMLS_CC);
  if (query) {
    POP_EO_PARAM();
    POP_PARAM();
    POP_PARAM();
  }

  limit.type = IS_LONG;
  limit.value.lval = 1;

  PUSH_PARAM(&limit); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoCursor_limit(1, cursor, &cursor, cursor, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM();

  zim_MongoCursor_getNext(0, return_value, return_value_ptr, cursor, return_value_used TSRMLS_CC);

  zend_objects_store_del_ref(cursor TSRMLS_CC);
  zval_ptr_dtor(&cursor);
}

PHP_METHOD(MongoCollection, update) {
  zval *criteria, *newobj;
  zend_bool upsert = 0;
  mongo_collection *c;
  mongo_link *link;
  mongo_msg_header header;
  CREATE_BUF(buf, INITIAL_BUF_SIZE);

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "aa|b", &criteria, &newobj, &upsert) == FAILURE) {
    return;
  }

  c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  ZEND_FETCH_RESOURCE2(link, mongo_link*, &c->db->link, -1, PHP_CONNECTION_RES_NAME, le_connection, le_pconnection); 

  CREATE_HEADER(buf, Z_STRVAL_P(c->ns), Z_STRLEN_P(c->ns), OP_UPDATE);
  serialize_int(&buf, upsert);
  zval_to_bson(&buf, criteria, NO_PREP TSRMLS_CC);
  zval_to_bson(&buf, newobj, NO_PREP TSRMLS_CC);
  serialize_size(buf.start, &buf);

  RETVAL_BOOL(mongo_say(link, &buf TSRMLS_CC)+1);
  efree(buf.start);
}

PHP_METHOD(MongoCollection, remove) {
  zval *criteria = 0;
  zend_bool just_one = 0;
  mongo_collection *c;
  mongo_link *link;
  int mflags;
  mongo_msg_header header;
  CREATE_BUF(buf, INITIAL_BUF_SIZE);

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|ab", &criteria, &just_one) == FAILURE) {
    return;
  }

  if (!criteria) {
    MAKE_STD_ZVAL(criteria);
    array_init(criteria);
  }
  else {
    zval_add_ref(&criteria);
  }

  c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  ZEND_FETCH_RESOURCE2(link, mongo_link*, &c->db->link, -1, PHP_CONNECTION_RES_NAME, le_connection, le_pconnection); 

  CREATE_HEADER(buf, Z_STRVAL_P(c->ns), Z_STRLEN_P(c->ns), OP_DELETE);

  mflags = (just_one == 1);

  serialize_int(&buf, mflags);
  zval_to_bson(&buf, criteria, NO_PREP TSRMLS_CC);
  serialize_size(buf.start, &buf);

  RETVAL_BOOL(mongo_say(link, &buf TSRMLS_CC)+1);

  efree(buf.start);
  zval_ptr_dtor(&criteria);
}

PHP_METHOD(MongoCollection, ensureIndex) {
  zval *keys, *db, *system_indexes, *collection, *data, *key_str;
  zend_bool unique;
  mongo_collection *c;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z|b", &keys, &unique) == FAILURE) {
    return;
  }

  if (Z_TYPE_P(keys) != IS_ARRAY) {
    zval *key_array;

    convert_to_string(keys);

    MAKE_STD_ZVAL(key_array);
    array_init(key_array);
    add_assoc_long(key_array, Z_STRVAL_P(keys), 1);

    keys = key_array;
  }
  else {
    zval_add_ref(&keys);
  }

  c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  // get the system.indexes collection
  db = c->parent;

  MAKE_STD_ZVAL(system_indexes);
  ZVAL_STRING(system_indexes, "system.indexes", 1);

  MAKE_STD_ZVAL(collection);

  PUSH_PARAM(system_indexes); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoDB_selectCollection(1, collection, &collection, db, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM();

  // set up data
  MAKE_STD_ZVAL(data);
  array_init(data);

  // ns
  add_assoc_zval(data, "ns", c->ns);
  zval_add_ref(&c->ns);
  add_assoc_zval(data, "key", keys);

  // turn keys into a string
  MAKE_STD_ZVAL(key_str);

  // MongoUtil::toIndexString()
  PUSH_PARAM(keys); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoUtil_toIndexString(1, key_str, &key_str, NULL, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM();

  add_assoc_zval(data, "name", key_str);
  add_assoc_bool(data, "unique", unique);

  // MongoCollection::insert()
  PUSH_PARAM(data); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoCollection_insert(1, return_value, return_value_ptr, collection, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM();

  zval_ptr_dtor(&data); 
  zval_ptr_dtor(&system_indexes);
  zval_ptr_dtor(&collection);
  zval_ptr_dtor(&keys);
}

PHP_METHOD(MongoCollection, deleteIndex) {
  zval *keys, *key_str, *data;
  mongo_collection *c;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &keys) == FAILURE) {
    return;
  }
  MAKE_STD_ZVAL(key_str);

  PUSH_PARAM(keys); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoUtil_toIndexString(1, key_str, &key_str, NULL, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM();

  c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  MAKE_STD_ZVAL(data);
  array_init(data);
  add_assoc_zval(data, "deleteIndexes", c->name);
  zval_add_ref(&c->name);
  zval_add_ref(&key_str);
  add_assoc_zval(data, "index", key_str);
 
  PUSH_PARAM(data); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoDB_command(1, return_value, return_value_ptr, c->parent, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM();

  zval_ptr_dtor(&data);
  zval_ptr_dtor(&key_str);
}

PHP_METHOD(MongoCollection, deleteIndexes) {
  zval *data;
  mongo_collection *c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  MAKE_STD_ZVAL(data);
  array_init(data);

  add_assoc_string(data, "deleteIndexes", Z_STRVAL_P(c->name), 1);
  add_assoc_string(data, "index", "*", 1);

  PUSH_PARAM(data); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoDB_command(1, return_value, return_value_ptr, c->parent, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM();

  zval_ptr_dtor(&data);
}

PHP_METHOD(MongoCollection, getIndexInfo) {
  zval *collection, *i_str, *query, *cursor, *next;
  mongo_collection *c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  MAKE_STD_ZVAL(collection);

  MAKE_STD_ZVAL(i_str);
  ZVAL_STRING(i_str, "system.indexes", 1);

  PUSH_PARAM(i_str); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoDB_selectCollection(1, collection, &collection, c->parent, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM();

  zval_ptr_dtor(&i_str);

  MAKE_STD_ZVAL(query);
  array_init(query);
  add_assoc_string(query, "ns", Z_STRVAL_P(c->ns), 1);

  MAKE_STD_ZVAL(cursor);

  PUSH_PARAM(query); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoCollection_find(1, cursor, &cursor, collection, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM();

  zval_ptr_dtor(&query);
  zval_ptr_dtor(&collection);

  array_init(return_value);

  MAKE_STD_ZVAL(next);
  zim_MongoCursor_getNext(0, next, &next, cursor, return_value_used TSRMLS_CC);
  while (Z_TYPE_P(next) != IS_NULL) {
    add_next_index_zval(return_value, next);

    MAKE_STD_ZVAL(next);
    zim_MongoCursor_getNext(0, next, &next, cursor, return_value_used TSRMLS_CC);
  }
  zval_ptr_dtor(&next);
  zval_ptr_dtor(&cursor);
}

PHP_METHOD(MongoCollection, count) {
  zval *response, *data;
  zval **n;
  mongo_collection *c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  MAKE_STD_ZVAL(response);

  MAKE_STD_ZVAL(data);
  array_init(data);
  add_assoc_string(data, "count", Z_STRVAL_P(c->name), 1);

  PUSH_PARAM(data); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoDB_command(1, response, &response, c->parent, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM();

  zval_ptr_dtor(&data);
  if (zend_hash_find(Z_ARRVAL_P(response), "n", 2, (void**)&n) == SUCCESS) {
    RETVAL_ZVAL(*n, 1, 0);
    zval_ptr_dtor(&response);
  }
  else {
    RETURN_ZVAL(response, 0, 0);
  }
}

PHP_METHOD(MongoCollection, save) {
  zval *a;
  zval **id;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "a", &a) == FAILURE) {
    return;
  }

  if (zend_hash_find(Z_ARRVAL_P(a), "_id", 4, (void**)&id) == SUCCESS) {
    zval zupsert;
    zval *criteria;

    MAKE_STD_ZVAL(criteria);
    array_init(criteria);
    add_assoc_zval(criteria, "_id", *id);

    Z_TYPE(zupsert) = IS_BOOL;
    zupsert.value.lval = 1;

    PUSH_PARAM(criteria); PUSH_PARAM(a); PUSH_PARAM(&zupsert); PUSH_PARAM((void*)3);
    PUSH_EO_PARAM();
    zim_MongoCollection_update(3, return_value, return_value_ptr, getThis(), return_value_used TSRMLS_CC);
    POP_EO_PARAM();
    POP_PARAM(); POP_PARAM(); POP_PARAM(); POP_PARAM();

    return;
  }
  
  PUSH_PARAM(a); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoCollection_insert(1, return_value, return_value_ptr, getThis(), return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM(); 
}

PHP_METHOD(MongoCollection, createDBRef) {
  zval *obj;
  mongo_collection *c;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &obj) == FAILURE) {
    return;
  }

  c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  PUSH_PARAM(c->name); PUSH_PARAM(obj); PUSH_PARAM((void*)2);
  PUSH_EO_PARAM();
  zim_MongoDB_createDBRef(2, return_value, return_value_ptr, c->parent, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM(); POP_PARAM(); 
}

PHP_METHOD(MongoCollection, getDBRef) {
  zval *ref;
  mongo_collection *c;

  if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &ref) == FAILURE) {
    return;
  }

  c = (mongo_collection*)zend_object_store_get_object(getThis() TSRMLS_CC);

  PUSH_PARAM(ref); PUSH_PARAM((void*)1);
  PUSH_EO_PARAM();
  zim_MongoDB_getDBRef(1, return_value, return_value_ptr, c->parent, return_value_used TSRMLS_CC);
  POP_EO_PARAM();
  POP_PARAM(); POP_PARAM(); 
}

static function_entry MongoCollection_methods[] = {
  PHP_ME(MongoCollection, __construct, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, __toString, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, getName, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, drop, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, validate, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, insert, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, batchInsert, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, update, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, remove, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, find, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, findOne, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, ensureIndex, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, deleteIndex, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, deleteIndexes, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, getIndexInfo, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, count, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, save, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, createDBRef, NULL, ZEND_ACC_PUBLIC)
  PHP_ME(MongoCollection, getDBRef, NULL, ZEND_ACC_PUBLIC)
  {NULL, NULL, NULL}
};

static void mongo_mongo_collection_free(void *object TSRMLS_DC) {
  mongo_collection *c = (mongo_collection*)object;

  if (c) {
    if (c->parent) {
      zval_ptr_dtor(&c->parent);
    }
    if (c->name) {
      zval_ptr_dtor(&c->name);
    }
    if (c->ns) {
      zval_ptr_dtor(&c->ns);
    }
    zend_object_std_dtor(&c->std TSRMLS_CC);
    efree(c);
  }
}


/* {{{ mongo_mongo_collection_new
 */
zend_object_value mongo_mongo_collection_new(zend_class_entry *class_type TSRMLS_DC) {
  zend_object_value retval;
  mongo_collection *intern;
  zval *tmp;

  intern = (mongo_collection*)emalloc(sizeof(mongo_collection));
  memset(intern, 0, sizeof(mongo_collection));

  zend_object_std_init(&intern->std, class_type TSRMLS_CC);
  zend_hash_copy(intern->std.properties, &class_type->default_properties, (copy_ctor_func_t) zval_add_ref, 
                 (void *) &tmp, sizeof(zval *));

  retval.handle = zend_objects_store_put(intern, (zend_objects_store_dtor_t) zend_objects_destroy_object, mongo_mongo_collection_free, NULL TSRMLS_CC);
  retval.handlers = &mongo_default_handlers;

  return retval;
}
/* }}} */

void mongo_init_MongoCollection(TSRMLS_D) {
  zend_class_entry ce;

  INIT_CLASS_ENTRY(ce, "MongoCollection", MongoCollection_methods);
  ce.create_object = mongo_mongo_collection_new;
  mongo_ce_Collection = zend_register_internal_class(&ce TSRMLS_CC);

  zend_declare_property_null(mongo_ce_Collection, "db", strlen("db"), ZEND_ACC_PUBLIC TSRMLS_CC);
}