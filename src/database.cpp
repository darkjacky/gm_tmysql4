#include "main.h"
#include "database.h"
#include "tmysql.h"

using namespace GarrysMod::Lua;

unsigned int database_index = 1;

Database::Database(std::string host, std::string user, std::string pass, std::string db, unsigned int port, std::string socket, unsigned long flags, int callback)
	: m_strHost(host), m_strUser(user), m_strPass(pass), m_strDB(db), m_iPort(port), m_strSocket(socket), m_iClientFlags(flags), m_iCallback(callback), m_bIsConnected(false), m_MySQL(NULL), m_iTableIndex(database_index++)
{
	work.reset(new asio::io_service::work(io_service));
}

Database::~Database( void )
{
}

bool Database::Initialize(std::string& error)
{
	m_MySQL = mysql_init(nullptr);

	if (m_MySQL == NULL)
	{
		error.assign("Out of memory!");
		return false;
	}

	if (!Connect(error))
		return false;

	thread_group.push_back(std::thread( [&]() { io_service.run(); } ));

	m_bIsConnected = true;
	return true;
}

bool Database::Connect(std::string& error)
{
	const char* socket = (m_strSocket.length() == 0) ? nullptr : m_strSocket.c_str();
	unsigned int flags = m_iClientFlags | CLIENT_MULTI_RESULTS;

	my_bool tru = 1;
	if (mysql_options(m_MySQL, MYSQL_OPT_RECONNECT, &tru) > 0)
	{
		error.assign(mysql_error(m_MySQL));
		return false;
	}

	std::string wkdir = get_working_dir() + "/garrysmod/lua/bin";
	if (mysql_options(m_MySQL, MYSQL_PLUGIN_DIR, wkdir.c_str()) > 0)
	{
		error.assign(mysql_error(m_MySQL));
		return false;
	}

	if (mysql_real_connect(m_MySQL, m_strHost.c_str(), m_strUser.c_str(), m_strPass.c_str(), m_strDB.c_str(), m_iPort, socket, flags) != m_MySQL)
	{
		error.assign(mysql_error(m_MySQL));
		return false;
	}

	m_bIsPendingCallback = true;

	return true;
}

void Database::Shutdown(void)
{
	work.reset();

	for (auto iter = thread_group.begin(); iter != thread_group.end(); ++iter)
		iter->join();

	assert(io_service.stopped());
}

std::size_t Database::RunShutdownWork(void)
{
	io_service.reset();
	return io_service.run();
}

void Database::Release(void)
{
	assert(io_service.stopped());

	if (m_MySQL != NULL)
	{
		mysql_close(m_MySQL);
		m_MySQL = NULL;
	}

	m_bIsConnected = false;
}

char* Database::Escape(const char* query, unsigned int len)
{
	char* escaped = new char[len * 2 + 1];
	mysql_real_escape_string(m_MySQL, escaped, query, len);
	return escaped;
}

bool Database::Option(mysql_option option, const char* arg, std::string& error)
{
	if (mysql_options(m_MySQL, option, arg) > 0)
	{
		error.assign(mysql_error(m_MySQL));
		return false;
	}

	return true;
}

const char* Database::GetServerInfo()
{
	return mysql_get_server_info(m_MySQL);
}

const char* Database::GetHostInfo()
{
	return mysql_get_host_info(m_MySQL);
}

int Database::GetServerVersion()
{
	return mysql_get_server_version(m_MySQL);
}

bool Database::SetCharacterSet(const char* charset, std::string& error)
{
	if (mysql_set_character_set(m_MySQL, charset) > 0)
	{
		error.assign(mysql_error(m_MySQL));
		return false;
	}

	return true;
}

void Database::QueueQuery(const char* query, int callback, int callbackref, bool usenumbers)
{
	Query* newquery = new Query(query, callback, callbackref, usenumbers);
	io_service.post(std::bind(&Database::RunQuery, this, newquery));
}

void Database::RunQuery(Query* query)
{
	std::string strquery = query->GetQuery();
	size_t len = strquery.length();

	bool hasRetried = false;

	retry:

	unsigned int errorno = NULL;
	if (mysql_real_query(m_MySQL, strquery.c_str(), len) != 0) {

		errorno = mysql_errno(m_MySQL);

		if (!hasRetried && ((errorno == CR_SERVER_LOST) || (errorno == CR_SERVER_GONE_ERROR) || (errorno != CR_CONN_HOST_ERROR) || (errorno == 1053)/*ER_SERVER_SHUTDOWN*/ || (errorno != CR_CONNECTION_ERROR))) {
			hasRetried = true;
			goto retry;
		}
	}
	
	int status = 0;
	while (status != -1) {
		query->AddResult(new Result(
			mysql_store_result(m_MySQL),
			errorno,
			mysql_error(m_MySQL),
			(double)mysql_affected_rows(m_MySQL),
			(double)mysql_insert_id(m_MySQL)));

		status = mysql_next_result(m_MySQL);
	}

	PushCompleted(query);
}

void Database::TriggerCallback(lua_State* state)
{
	m_bIsPendingCallback = false;

	if (GetCallback() >= 0)
	{
		LUA->ReferencePush(GetCallback());

		if (!LUA->IsType(-1, Type::Function))
		{
			LUA->Pop();
			return;
		}

		PushHandle(state);

		if (LUA->PCall(1, 0, 0))
		{
			LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);
			LUA->GetField(-1, "ErrorNoHalt"); // could cache this function... but this really should not be called in the first place
			LUA->Push(-3); // This is the error message from PCall
			LUA->PushString("\n"); // add a newline since ErrorNoHalt does not do that itself
			LUA->Call(2, 0);
			LUA->Pop(2); // Pop twice since the PCall error is still on the stack
		}
	}
}

void Database::PushHandle(lua_State* state)
{
	UserData* userdata = (UserData*)LUA->NewUserdata(sizeof(UserData));
	userdata->data = this;
	userdata->type = DATABASE_MT_ID;

	LUA->ReferencePush(tmysql::iRefDatabases);
	LUA->PushNumber(GetTableIndex());
	LUA->Push(-3);
	LUA->SetTable(-3);
	LUA->Pop();

	LUA->CreateMetaTableType(DATABASE_MT_NAME, DATABASE_MT_ID);
	LUA->SetMetaTable(-2);
}

void Database::Disconnect(lua_State* state)
{
	Shutdown();

	DispatchCompletedQueries(state);

	while (RunShutdownWork())
		DispatchCompletedQueries(state);

	Release();

	delete this;
}

void Database::DispatchCompletedQueries(lua_State* state)
{
	Query* completed = GetCompletedQueries();

	while (completed)
	{
		Query* query = completed;

		if (!tmysql::inShutdown)
			query->TriggerCallback(state);

		completed = query->next;
		delete query;
	}
}


#pragma region Lua Exports
int Database::lua_IsValid(lua_State* state)
{
	LUA->CheckType(1, DATABASE_MT_ID);

	Database* mysqldb = *reinterpret_cast<Database**>(LUA->GetUserdata(1));

	LUA->PushBool(mysqldb != NULL);

	return 1;
}

int Database::lua_Query(lua_State* state)
{
	LUA->CheckType(1, DATABASE_MT_ID);

	Database* mysqldb = *reinterpret_cast<Database**>(LUA->GetUserdata(1));

	if (!mysqldb) {
		LUA->ThrowError("Attempted to call Query on a shutdown database");
		return 0;
	}

	if (!mysqldb->IsConnected()) {
		LUA->ThrowError("Attempted to call Query on a disconnected database");
		return 0;
	}

	const char* query = LUA->CheckString(2);

	int callbackfunc = -1;
	if (LUA->GetType(3) == Type::Function)
	{
		LUA->Push(3);
		callbackfunc = LUA->ReferenceCreate();
	}

	int callbackref = -1;
	int callbackobj = LUA->GetType(4);
	if (callbackobj != Type::Nil)
	{
		LUA->Push(4);
		callbackref = LUA->ReferenceCreate();
	}

	mysqldb->QueueQuery(query, callbackfunc, callbackref, LUA->GetBool(5));
	return 0;
}

int Database::lua_Escape(lua_State* state)
{
	LUA->CheckType(1, DATABASE_MT_ID);

	Database* mysqldb = *reinterpret_cast<Database**>(LUA->GetUserdata(1));

	if (!mysqldb) {
		LUA->ThrowError("Attempted to call Escape on a shutdown database");
		return 0;
	}

	if (!mysqldb->IsConnected()) {
		LUA->ThrowError("Attempted to call Escape on a disconnected database");
		return 0;
	}

	LUA->CheckType(2, Type::String);

	unsigned int len;
	const char* query = LUA->GetString(2, &len);

	char* escaped = mysqldb->Escape(query, len);
	LUA->PushString(escaped);

	delete[] escaped;
	return 1;
}

int Database::lua_SetOption(lua_State* state)
{
	LUA->CheckType(1, DATABASE_MT_ID);

	Database* mysqldb = *reinterpret_cast<Database**>(LUA->GetUserdata(1));

	if (!mysqldb) {
		LUA->ThrowError("Attempted to call Option on a shutdown database");
		return 0;
	}

	std::string error;

	mysql_option option = static_cast<mysql_option>((int)LUA->CheckNumber(2));

	LUA->PushBool(mysqldb->Option(option, LUA->CheckString(3), error));
	LUA->PushString(error.c_str());
	return 2;
}

int Database::lua_GetServerInfo(lua_State* state)
{
	LUA->CheckType(1, DATABASE_MT_ID);

	Database* mysqldb = *reinterpret_cast<Database**>(LUA->GetUserdata(1));

	if (!mysqldb) {
		LUA->ThrowError("Attempted to call GetServerInfo on a shutdown database");
		return 0;
	}

	if (!mysqldb->IsConnected()) {
		LUA->ThrowError("Attempted to call GetServerInfo on a disconnected database");
		return 0;
	}

	LUA->PushString(mysqldb->GetServerInfo());
	return 1;
}

int Database::lua_GetHostInfo(lua_State* state)
{
	LUA->CheckType(1, DATABASE_MT_ID);

	Database* mysqldb = *reinterpret_cast<Database**>(LUA->GetUserdata(1));

	if (!mysqldb) {
		LUA->ThrowError("Attempted to call GetHostInfo on a shutdown database");
		return 0;
	}

	if (!mysqldb->IsConnected()) {
		LUA->ThrowError("Attempted to call GetHostInfo on a disconnected database");
		return 0;
	}

	LUA->PushString(mysqldb->GetHostInfo());
	return 1;
}

int Database::lua_GetServerVersion(lua_State* state)
{
	LUA->CheckType(1, DATABASE_MT_ID);

	Database* mysqldb = *reinterpret_cast<Database**>(LUA->GetUserdata(1));

	if (!mysqldb) {
		LUA->ThrowError("Attempted to call GetServerVersion on a shutdown database");
		return 0;
	}

	if (!mysqldb->IsConnected()) {
		LUA->ThrowError("Attempted to call GetServerVersion on a disconnected database");
		return 0;
	}

	LUA->PushNumber(mysqldb->GetServerVersion());
	return 1;
}

int Database::lua_Connect(lua_State* state)
{
	LUA->CheckType(1, DATABASE_MT_ID);

	Database* mysqldb = *reinterpret_cast<Database**>(LUA->GetUserdata(1));

	if (!mysqldb) {
		LUA->ThrowError("Attempted to call Connect on a shutdown database");
		return 0;
	}

	if (mysqldb->IsConnected()) {
		LUA->ThrowError("Attempted to call Connect on an already connected database");
		return 0;
	}

	std::string error;
	bool success = mysqldb->Initialize(error);

	LUA->PushBool(success);

	if (!success)
	{
		LUA->PushString(error.c_str());
		delete mysqldb;
		return 2;
	}
	return 1;
}

int Database::lua_IsConnected(lua_State* state)
{
	LUA->CheckType(1, DATABASE_MT_ID);

	Database* mysqldb = *reinterpret_cast<Database**>(LUA->GetUserdata(1));

	if (!mysqldb) {
		LUA->ThrowError("Attempted to call IsConnected on a shutdown database");
		return 0;
	}

	LUA->PushBool(mysqldb->IsConnected());
	return 1;
}

int Database::lua_Disconnect(lua_State* state)
{
	LUA->CheckType(1, DATABASE_MT_ID);

	Database* mysqldb = *reinterpret_cast<Database**>(LUA->GetUserdata(1));

	if (!mysqldb) {
		LUA->ThrowError("Attempted to call Disconnect on a shutdown database");
		return 0;
	}

	LUA->ReferencePush(tmysql::iRefDatabases);
	LUA->PushNumber(mysqldb->GetTableIndex());
	LUA->PushNil();
	LUA->SetTable(-3);

	mysqldb->Disconnect(state);

	return 0;
}

int Database::lua_SetCharacterSet(lua_State* state)
{
	LUA->CheckType(1, DATABASE_MT_ID);

	Database* mysqldb = *reinterpret_cast<Database**>(LUA->GetUserdata(1));

	if (!mysqldb) {
		LUA->ThrowError("Attempted to call SetCharacterSet on a shutdown database");
		return 0;
	}

	if (!mysqldb->IsConnected()) {
		LUA->ThrowError("Attempted to call SetCharacterSet on a disconnected database");
		return 0;
	}

	const char* set = LUA->CheckString(2);

	std::string error;
	LUA->PushBool(mysqldb->SetCharacterSet(set, error));
	LUA->PushString(error.c_str());
	return 2;
}

int Database::lua_Poll(lua_State* state)
{
	LUA->CheckType(1, DATABASE_MT_ID);

	Database* mysqldb = *reinterpret_cast<Database**>(LUA->GetUserdata(1));

	if (!mysqldb) {
		LUA->ThrowError("Attempted to call Poll on a shutdown database");
		return 0;
	}

	if (mysqldb->IsPendingCallback())
		mysqldb->TriggerCallback(state);

	mysqldb->DispatchCompletedQueries(state);
	return 0;
}
#pragma endregion