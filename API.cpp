/*
*   A bastard child of C and C++, this is where the magic happens. Websockets etc.
*/
#ifndef _CRT_SECURE_NO_WARNINGS
# define _CRT_SECURE_NO_WARNINGS 
#endif
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
# define _WINSOCK_DEPRECATED_NO_WARNINGS  
#endif
#include "API.h"
#include "plugin.h"

#include "dependencies/webby/webby.c"

TS3_API::TS3_API() {
    wsa_data.wVersion = 0;
    // Runner thread to keep the websocket up-to-date without a third party update loop
    runner = std::thread([]() {
        TS3_API& instance = Instance();
        while (!instance.shutting_down) {
            instance.update();
            Sleep(20);
        }
        });
    startWebserver();
};
TS3_API::~TS3_API() {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    shutting_down = true;
    stopWebserver();

    if (runner.joinable())
        runner.join();
}
void TS3_API::resetServerVariables() {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    server.reset();
    for (auto& client : clients) {
        delete client.second;
    }
    clients.clear();
}    

int TS3_API::removeClient(struct WebbyConnection* connection) {
    for (int i = 0; i < ws_connection_count; i++)
    {
        if (ws_connections[i] == connection)
        {
            int remain = ws_connection_count - i;
            memmove(ws_connections + i, ws_connections + i + 1, remain * sizeof(struct WebbyConnection*));
            --ws_connection_count;
            return 0;
        }
    }
    return 1;
}
int TS3_API::addClient(struct WebbyConnection* connection) {
    ws_connections[ws_connection_count++] = connection;
    return 0;
}
int TS3_API::stopWebserver() {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if (ws_server) {
        WebbyServerUpdate(ws_server);
        WebbyServerShutdown(ws_server);
        ws_server = nullptr;
    }
    if (ws_memory) {
        free(ws_memory);
        ws_memory = nullptr;
    }
    if (wsa_data.wVersion) {
        WSACleanup();
    }
    ws_connection_count = 0;
    return 0;
}
void TS3_API::onWebsocketDebugMessage(const char* message) {
    onLogMessage("Websocket Debug: %s",message);
}
int TS3_API::sendServerInfo() {
    // todo
    return 0;
}
void TS3_API::onLogMessage(const char* message, ...) {
    char buf[255];
    size_t msgLen = strlen(strncpy(buf, message, sizeof(buf) - 2));
    if (buf[msgLen - 1] != '\n')
        buf[msgLen] = '\n';
    char buf2[512];
    va_list args;
    va_start(args, message);
    vsnprintf(buf2, 512, buf, args);
    va_end(args);
    OutputDebugString(buf2);
}
int TS3_API::onHttpRequest(struct WebbyConnection* connection) {
    onLogMessage("onHttpRequest unsupported");
    char buf[1024];
    int read_len = connection->request.content_length;
    if (read_len > sizeof(buf) - 1)
        read_len = sizeof(buf) - 1;
    if (WebbyRead(connection, buf, read_len))
        return 1;
    Instance().sendAllInfo(connection);
    return 0;
}
int TS3_API::onWebsocketConnection(struct WebbyConnection* connection) {
    return 0; // Allow all
}
int TS3_API::writeJson(struct WebbyConnection* connection, std::string& message) {
    WebbyHeader headers[1] = { {"Content-Type","application/json"} };
    return write(connection, message.c_str(), headers, 1);
}
int TS3_API::writeJson(struct WebbyConnection* connection, nlohmann::json & json) {
    std::string message(json.dump());
    return writeJson(connection, message);
}
bool TS3_API::isWebsocketConnection(struct WebbyConnection* connection) {
    auto& request = connection->request;
    for (int i = 0; i < request.header_count; i++) {
        if (strstr(request.headers[i].name, "WebSocket") != NULL)
            return true;
    }
    return false;
}
int TS3_API::write(struct WebbyConnection* connection, const char* response, struct WebbyHeader* headers, int header_count) {
    size_t response_len = strlen(response);
    if (is_websocket_request(connection)) {
        WebbyBeginSocketFrame(connection, WEBBY_WS_OP_TEXT_FRAME);
        WebbyPrintf(connection, "%s\r\n", response);
        WebbyEndSocketFrame(connection);
        return 0;
    }
    else {
        size_t response_len = strlen(response);
        if (WebbyBeginResponse(connection, 200, static_cast<int>(response_len), headers, header_count))
            return 1;
        int ok = WebbyWrite(connection, response, response_len);
        WebbyEndResponse(connection);
        return ok;
    }
}
void TS3_API::onWebsocketConnected(struct WebbyConnection* connection) {
    onLogMessage("onWebsocketConnected %s", connection->request.uri);
    if (Instance().addClient(connection))
        onLogMessage("Failed to add client!");
    Instance().sendAllInfo(connection);
}
void TS3_API::onWebsocketDisconnect(struct WebbyConnection* connection) {
    onLogMessage("onWebsocketDisconnect");
    if (Instance().removeClient(connection))
        onLogMessage("Failed to remove client!");
}
int TS3_API::onClientMessage(const char* message, struct WebbyConnection* connection) {
    onLogMessage("onClientMessage");

    return 0;
}
int TS3_API::onWebsocketMessage(struct WebbyConnection* connection, const struct WebbyWsFrame* frame)
{
    int i = 0;

    /*printf("WebSocket frame incoming\n");
    printf("  Frame OpCode: %d\n", frame->opcode);
    printf("  Final frame?: %s\n", (frame->flags & WEBBY_WSF_FIN) ? "yes" : "no");
    printf("  Masked?     : %s\n", (frame->flags & WEBBY_WSF_MASKED) ? "yes" : "no");
    printf("  Data Length : %d\n", (int)frame->payload_length);*/

    char message[1024];
    if (frame->payload_length > sizeof(message) - 1)
        return 0;
    size_t message_len = 0;
    while (i < frame->payload_length)
    {
        unsigned char buffer[16];
        int remain = (int)(frame->payload_length - i);
        int read_size = remain > (int) sizeof buffer ? sizeof buffer : remain;
        int k;
        if (0 != WebbyRead(connection, buffer, read_size))
            break;
        for (k = 0; k < read_size; ++k) {
            message[message_len++] = buffer[k];
        }
        i += read_size;
    }
    message[message_len] = 0;
    return Instance().onClientMessage(message, connection);
}
int TS3_API::startWebserver() {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    stopWebserver();

    int memory_size;
    struct WebbyServerConfig config;
#if defined(_WIN32)
    {
        WORD wsa_version = MAKEWORD(2, 2);
        if (wsa_data.wVersion == 0 && 0 != WSAStartup(wsa_version, &wsa_data))
        {
            printf("WSAStartup failed\n");
            return 1;
        }
    }
#endif
    memset(&config, 0, sizeof config);
    config.bind_address = "127.0.0.1";
    config.listening_port = 27032;
    config.flags = WEBBY_SERVER_WEBSOCKETS | WEBBY_SERVER_LOG_DEBUG;
    config.connection_max = 4;
    config.request_buffer_size = 2048;
    config.io_buffer_size = 8192;
    config.dispatch = &onHttpRequest;
    config.log = &onWebsocketDebugMessage;
    config.ws_connect = &onWebsocketConnection;
    config.ws_connected = &onWebsocketConnected;
    config.ws_closed = &onWebsocketDisconnect;
    config.ws_frame = &onWebsocketMessage;

    memory_size = WebbyServerMemoryNeeded(&config);
    ws_memory = malloc(memory_size);
    ws_server = WebbyServerInit(&config, ws_memory, memory_size);

    if (!ws_server)
    {
        fprintf(stderr, "failed to init server\n");
        stopWebserver();
        return 1;
    }
    printf("Websocket server started on %s:%d\n", config.bind_address, config.listening_port);
    return 0;
}
// changed_bool set if the user object has been modified i.e. different from before
TS3_API::TS3User* TS3_API::updateUser(anyID clientId, TS3User* client, bool* changed_bool) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    auto found = clients.find(clientId);
    if (found != clients.end()) {
        if (found->second == client) {
            if (changed_bool)
                *changed_bool = false;
            return client; // Its the same object, so same values.
        }
        auto* existing = found->second;
        // @Performance: Maybe remove string comparison if its not needed
        if(changed_bool)
            *changed_bool = memcmp(client, existing, sizeof(TS3_API::TS3User)) != 0;
        memcpy(existing, client, sizeof(TS3_API::TS3User));
        delete client;
        client = existing;
    }
    else {
        client->id = clientId;
        clients.emplace(clientId, client);
        if (changed_bool)
            *changed_bool = true;
    }
    return client;
}
int TS3_API::deleteUser(anyID clientId) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    auto found = clients.find(clientId);
    if (found == clients.end())
        return 1;
    delete found->second;
    clients.erase(found);
    sendAllInfo();
    return 0;
}
int TS3_API::sendAllInfo(struct WebbyConnection* connection) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    nlohmann::json jsonClients;
    nlohmann::json jsonBody;
    for (auto& it : clients) {
        TS3User* client = it.second;
        if (!client) continue;
        nlohmann::json clientJson;
        if (client->toJson(&clientJson)) continue;
        jsonClients.push_back(clientJson);
    }
    jsonBody["clients"] = jsonClients;
    nlohmann::json jsonServer;
    server.toJson(&jsonServer);
    jsonBody["server"] = jsonServer;
    if (!connection) {
        // Send to all connected sockets.
        for (int i = 0; i < ws_connection_count; i++) {
            if (!ws_connections[i]) continue;
            writeJson(ws_connections[i], jsonBody);
        }
    }
    else {
        // Only send to this socket.
        writeJson(connection, jsonBody);
    }
    return 0;
}
int TS3_API::onTeamspeakMessage(TS3User* from, TS3User* to, const char* message) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    nlohmann::json json;
    nlohmann::json fromJson;
    nlohmann::json toJson;
    json["message"] = message;
    from->toJson(&fromJson);
    json["from"] = fromJson;
    if (to) {
        to->toJson(&toJson);
        json["to"] = toJson;
    }
    // Send to all connected sockets.
    std::string json_body(json.dump());
    for (int i = 0; i < ws_connection_count; i++) {
        if (!ws_connections[i]) continue;
        writeJson(ws_connections[i], json_body);
    }
    return 0;
}
int TS3_API::sendAllClients(struct WebbyConnection* connection) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    nlohmann::json jsonArray;
    for (auto it : clients) {
        TS3User* client = it.second;
        if (!client) continue;
        nlohmann::json clientJson;
        if (client->toJson(&clientJson)) continue;
        jsonArray.push_back(clientJson);
    }
    std::string message(jsonArray.dump());
    size_t len = message.length();
    if (!connection) {
        // Send to all connected sockets.
        for (int i = 0; i < ws_connection_count; i++) {
            if (!ws_connections[i]) continue;
            write(ws_connections[i], message.c_str());
        }
    }
    else {
        // Only send to this socket.
        write(connection, message.c_str());
    }
    return 0;
}
int TS3_API::sendClient(anyID clientId, struct WebbyConnection* connection) {
    TS3User* client = getUser(clientId);
    if (!client)
        return 1;
    nlohmann::json clientJson;
    if (client->toJson(&clientJson))
        return 1;
    nlohmann::json jsonArray;
    jsonArray.push_back(clientJson);
    std::string message(jsonArray.dump());
    size_t len = message.length();
    if (!connection) {
        // Send to all connected sockets.
        for (int i = 0; i < ws_connection_count; i++) {
            if (!ws_connections[i]) continue;
            write(ws_connections[i], message.c_str());
        }
    }
    else {
        // Only send to this socket.
        write(connection, message.c_str());
    }
    return 0;
}
void TS3_API::update() {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    if (ws_server)
        WebbyServerUpdate(ws_server);
    static clock_t last_update = 0;
    if (!last_update)
        last_update = clock();
    if (clock() - last_update > 2 * CLOCKS_PER_SEC) {
        last_update = clock();
        refreshAll();
    }
}
TS3_API::TS3User* TS3_API::getUser(anyID clientId) {
    std::lock_guard<std::recursive_mutex> guard(mutex);
    auto found = clients.find(clientId);
    if (found == clients.end())
        return nullptr;
    return found->second;
}
int TS3_API::TS3User::toJson(nlohmann::json* json) {
    json->emplace("name", name);
    json->emplace("id", id);
    json->emplace("channel", channel_id);
    json->emplace("is_talking", is_talking);
    json->emplace("mic_muted", mic_muted);
    json->emplace("speakers_muted", speakers_muted);
    return 0;
}
int TS3_API::TS3Server::toJson(nlohmann::json* json) {
    json->emplace("name", name);
    json->emplace("host", host);
    json->emplace("port", port);
    json->emplace("password", password);
    json->emplace("id", id);
    json->emplace("my_id", my_client_id);
    return 0;
}
void TS3_API::TS3Server::reset() {
    name[0] = 0;
    id = 0;
    my_client_id = 0;
}