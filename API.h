#pragma once


#include <cstdio>
#include <cstdlib>

#include <thread>
#include <mutex>

#ifdef _WIN32
#pragma comment( lib, "WSock32.Lib" )
#include <winsock2.h>
#endif
#include <map>
#include "teamspeak/public_definitions.h"
#include "dependencies/nlohmann/json.hpp"

class TS3_API {
    TS3_API();
    ~TS3_API();
public:
    struct TS3User {
        char name[256];
        anyID id = 0;
        uint64 channel_id = 0;
        char is_talking = 0;
        char speakers_muted = 0;
        char mic_muted = 0;
        int toJson(nlohmann::json* json);
    };
    struct TS3Server {
        uint64 id;
        anyID my_client_id;
        char name[256];
        char welcome_message[512];
        char host[256];
        unsigned short port = 0;
        char password[128];
        int toJson(nlohmann::json* json);
        void reset();
    } server;
    int is_websocket_connection = 1; // Used for identifying websocket connections in request->user_data
private:
    enum {
        MAX_WSCONN = 5
    };
    std::thread runner;
    std::recursive_mutex mutex;
    int shutting_down = false;
    int last_error_code = 0;
    int ws_connection_count = 0;
    WSADATA wsa_data;
    struct WebbyConnection* ws_connections[MAX_WSCONN];
    void* ws_memory = nullptr;
    struct WebbyServer* ws_server = nullptr;

    std::map<anyID, TS3User*> clients;

    int removeClient(struct WebbyConnection* connection);
    int addClient(struct WebbyConnection* connection);
    int stopWebserver();
    static void onWebsocketDebugMessage(const char* message);

    // Raw packet received via HTTP request
    static int onHttpRequest(struct WebbyConnection* connection);
    // Raw connection started from a websocket client
    static int onWebsocketConnection(struct WebbyConnection* connection);
    // Client successfully connected via websocket
    static void onWebsocketConnected(struct WebbyConnection* connection);
    // Client disconnected via websocket
    static void onWebsocketDisconnect(struct WebbyConnection* connection);
    // Packet has been collated into a message; process it.
    static int onClientMessage(const char* message, struct WebbyConnection* connection);
    // Raw packet frame received via websocket
    static int onWebsocketMessage(struct WebbyConnection* connection, const struct WebbyWsFrame* frame);
    // Spin up the server!
    int startWebserver();
    // Send JSON object of current state of play
    int sendServerInfo();
    int writeJson(struct WebbyConnection* connection, std::string& message);
    int writeJson(struct WebbyConnection* connection, nlohmann::json & json);
    // Write out to a connection
    int write(struct WebbyConnection* connection, const char* response, struct WebbyHeader* headers = nullptr, int header_count = 0);
    int writeHttp(struct WebbyConnection* connection, const char* response, struct WebbyHeader* headers = nullptr, int header_count = 0);
    bool isWebsocketConnection(struct WebbyConnection* connection);
public:
    static TS3_API& Instance() {
        static TS3_API instance;
        return instance;
    }
    void resetServerVariables();
    // Call once per frame
    void update();
    // Log message recieved from Webby
    static void onLogMessage(const char* message, ...);
    TS3User* getUser(anyID clientId);
    // Update (or add) a client.
    TS3User* updateUser(anyID clientId, TS3User* client, bool* changed_bool = nullptr);
    // Remove a client.
    int deleteUser(anyID clientId);
    // Send a specific client JSON message to any connected sockets
    int sendClient(anyID clientId, struct WebbyConnection* connection = nullptr);
    // Send all clients in a JSON array to any connected sockets
    int sendAllClients(struct WebbyConnection* connection = nullptr);
    // Send all info including server data
    int sendAllInfo(struct WebbyConnection* connection = nullptr);
    // Trigger when message is received
    int onTeamspeakMessage(TS3User* from, TS3User* to, const char* message);
};