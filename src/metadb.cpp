#include <deque>
#include <map>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdio.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sqlite3.h>

using namespace std;

#include "easylogging++.h"
#include "config.hpp"
#include "cvedia.hpp"
#include "api.hpp"
#include "metadb.hpp"

/*
metadata
	key -> value
	cli_version : 0.1
	cli_arguments : -n name -b 100 ...
fields
	id : field_name : type
	0 : source : image
	1 : label : string
files
	file_id : file_name
	0 : train.csv
	1 : test.csv
hashes
	api_hash : file_id : record_hash
*/

MetaDb::MetaDb() {

	LOG(INFO) << "Initializing MetaDb";
	LOG(INFO) << "SQlite3 version " << SQLITE_VERSION << " (" << SQLITE_VERSION_NUMBER << ")";

	assert( sqlite3_libversion_number()==SQLITE_VERSION_NUMBER );
	assert( strcmp(sqlite3_sourceid(),SQLITE_SOURCE_ID)==0 );
	assert( strcmp(sqlite3_libversion(),SQLITE_VERSION)==0 );

}

MetaDb::~MetaDb() {

	sqlite3_finalize(stmt_api_hash_select);
    sqlite3_finalize(stmt_hash_insert);
}

int MetaDb::NewDb(const string db_file) {

	char *zErrMsg = 0;
	char *sql;

	// Remove old db is one exists
	remove(db_file.c_str());

	// Open database 
	int rc = sqlite3_open(db_file.c_str(), &db);

	// Create SQL statement
	sql = "CREATE TABLE metadata (key varchar, value varchar);	\
	CREATE TABLE hashes (api_hash BLOB, record_hash BLOB);	\
	CREATE UNIQUE INDEX idx_api_hash ON hashes (api_hash);";

	// Execute SQL statement
	rc = sqlite3_exec(db, sql, NULL, 0, &zErrMsg);
	if (rc != SQLITE_OK){
		LOG(ERROR) << "SQL error: " << zErrMsg;
		sqlite3_free(zErrMsg);
	} else {
		LOG(DEBUG) << "Table structure created";
	}

	PrepareStatements();
	SetPragmaOptions();

	return rc;
}

void MetaDb::PrepareStatements() {

	string sql = "SELECT COUNT(*) AS cnt FROM hashes WHERE api_hash = (?)";
	sqlite3_prepare(db, sql.c_str(), -1, &stmt_api_hash_select, 0);

	sql = "INSERT INTO hashes VALUES (?, ?)";
	sqlite3_prepare(db, sql.c_str(), -1, &stmt_hash_insert, 0);
}

void MetaDb::SetPragmaOptions() {

	char *zErrMsg = 0;

	sqlite3_exec(db, "PRAGMA synchronous=OFF", NULL, 0, &zErrMsg);
}

int MetaDb::LoadDb(const string db_file) {

	struct stat buffer;   
	bool exists = (stat(db_file.c_str(), &buffer) == 0); 

	if (!exists) {
		LOG(ERROR) << "Could not find Meta Db";
		return 0;
	}

	// Open database 
	int rc = sqlite3_open(db_file.c_str(), &db);

	if (rc){
		LOG(ERROR) << "Can't open database: " << sqlite3_errmsg(db);
		return 0;
	} else {
		LOG(INFO) << "Loaded database " << db_file;
	}

	PrepareStatements();
	SetPragmaOptions();
	
	return 1;
}

void MetaDb::InsertHash(string api_hash, string record_hash) {

	char* zErrMsg = 0;

	// Convert hashes to binary data
	int api_len = api_hash.size();
	int rec_len = record_hash.size();

	if (api_len % 2 != 0 || rec_len % 2 != 0) {
		LOG(ERROR) << "Hashes need to be in hexadecimal text format: A09E45...";
	}

	unsigned char api_byte_hash[api_len/2];
	unsigned char record_byte_hash[rec_len/2];

	// Convert the API hash
	for (int count = 0; count < api_len; count+=2) {
		sscanf(&api_hash.c_str()[count], "%2hhx", &api_byte_hash[count/2]);
	}
	// Convert the record hash
	for (int count = 0; count < api_len; count+=2) {
		sscanf(&record_hash.c_str()[count], "%2hhx", &record_byte_hash[count/2]);
	}

	sqlite3_bind_blob(stmt_hash_insert, 1, api_byte_hash, api_len/2, SQLITE_STATIC);
    sqlite3_bind_blob(stmt_hash_insert, 2, record_byte_hash, rec_len/2, SQLITE_STATIC);

    sqlite3_step(stmt_hash_insert);

	sqlite3_clear_bindings(stmt_hash_insert);
	sqlite3_reset(stmt_hash_insert);
}

void MetaDb::InsertMeta(string key, string value) {

	char* zErrMsg = 0;

	string sql = "INSERT INTO metadata VALUES ('" + key + "','" + value + "')";

	int rc = sqlite3_exec(db, sql.c_str(), NULL, 0, &zErrMsg);
	if( rc != SQLITE_OK ){
		LOG(ERROR) << "SQL error: " << zErrMsg;
		sqlite3_free(zErrMsg);
	}
}

bool MetaDb::HasApiHash(string hash) {

	int rec_count = 0;

	// Convert hashes to binary data
	int hash_len = hash.size();

	if (hash_len % 2 != 0) {
		LOG(ERROR) << "Hashes need to be in hexadecimal text format: A09E45...";
	}

	unsigned char byte_hash[hash_len/2];

	// Convert the API hash
	for (int count = 0; count < hash_len; count+=2) {
		sscanf(&hash.c_str()[count], "%2hhx", &byte_hash[count/2]);
	}

	sqlite3_bind_blob(stmt_api_hash_select, 1, byte_hash, hash_len/2, SQLITE_STATIC);

	int rc = sqlite3_step(stmt_api_hash_select);

	if (rc == SQLITE_ROW)
	{
		rec_count = sqlite3_column_int(stmt_api_hash_select, 0);
	}

	sqlite3_clear_bindings(stmt_api_hash_select);
	sqlite3_reset(stmt_api_hash_select);

	if (rec_count > 0)
		return true;
	else
		return false;
}

string MetaDb::GetMeta(string key) {

	sqlite3_stmt* stmt = NULL;
	string value;

	string sql = "SELECT value FROM metadata WHERE key = '" + key + "' LIMIT 1";

	int rc = sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		LOG(ERROR) << "Failed to prepare " << sql;
		return "";
	}

	int rowCount = 0;
	rc = sqlite3_step(stmt);

	if (rc != SQLITE_DONE && rc != SQLITE_OK)
	{
		char* valChar = (char* )sqlite3_column_text(stmt, 0);

		value = string(valChar);

		free(valChar);
	} else {
		value = "";
	}

	rc = sqlite3_finalize(stmt);

	return value;
}