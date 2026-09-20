#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "rkllm.h"

namespace {

std::string g_response;

int onResult(RKLLMResult* result, void*, LLMCallState state)
{
    if (state == RKLLM_RUN_NORMAL && result != nullptr && result->text != nullptr)
        g_response += result->text;
    return 0;
}

std::string base64Encode(const std::string& input)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((input.size() + 2) / 3 * 4);

    for (size_t offset = 0; offset < input.size(); offset += 3) {
        const unsigned int first = static_cast<unsigned char>(input[offset]);
        const unsigned int second = offset + 1 < input.size()
            ? static_cast<unsigned char>(input[offset + 1]) : 0;
        const unsigned int third = offset + 2 < input.size()
            ? static_cast<unsigned char>(input[offset + 2]) : 0;
        const unsigned int value = (first << 16U) | (second << 8U) | third;

        output += alphabet[(value >> 18U) & 0x3fU];
        output += alphabet[(value >> 12U) & 0x3fU];
        output += offset + 1 < input.size() ? alphabet[(value >> 6U) & 0x3fU] : '=';
        output += offset + 2 < input.size() ? alphabet[value & 0x3fU] : '=';
    }
    return output;
}

int base64Value(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A';
    if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9')
        return ch - '0' + 52;
    if (ch == '+')
        return 62;
    if (ch == '/')
        return 63;
    return -1;
}

bool base64Decode(const std::string& input, std::string& output)
{
    if (input.empty() || input.size() % 4 != 0)
        return false;

    output.clear();
    output.reserve(input.size() / 4 * 3);
    for (size_t offset = 0; offset < input.size(); offset += 4) {
        const int first = base64Value(static_cast<unsigned char>(input[offset]));
        const int second = base64Value(static_cast<unsigned char>(input[offset + 1]));
        const int third = input[offset + 2] == '=' ? -2 : base64Value(static_cast<unsigned char>(input[offset + 2]));
        const int fourth = input[offset + 3] == '=' ? -2 : base64Value(static_cast<unsigned char>(input[offset + 3]));
        if (first < 0 || second < 0 || third == -1 || fourth == -1 ||
            (third == -2 && fourth != -2) ||
            ((third == -2 || fourth == -2) && offset + 4 != input.size())) {
            return false;
        }

        const unsigned int value = (static_cast<unsigned int>(first) << 18U) |
            (static_cast<unsigned int>(second) << 12U) |
            (static_cast<unsigned int>(third < 0 ? 0 : third) << 6U) |
            static_cast<unsigned int>(fourth < 0 ? 0 : fourth);
        output += static_cast<char>((value >> 16U) & 0xffU);
        if (third != -2)
            output += static_cast<char>((value >> 8U) & 0xffU);
        if (fourth != -2)
            output += static_cast<char>(value & 0xffU);
    }
    return true;
}

bool readLine(int fd, std::string& line)
{
    line.clear();
    while (line.size() < 128 * 1024) {
        char ch = '\0';
        const ssize_t readSize = recv(fd, &ch, 1, 0);
        if (readSize == 0)
            return false;
        if (readSize < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (ch == '\n')
            return true;
        line += ch;
    }
    return false;
}

bool writeLine(int fd, const std::string& line)
{
    const std::string message = line + '\n';
    size_t offset = 0;
    while (offset < message.size()) {
        const ssize_t written = send(fd, message.data() + offset, message.size() - offset, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool runPrompt(LLMHandle handle, const std::string& prompt, std::string& response)
{
    if (rkllm_clear_kv_cache(handle, 0, nullptr, nullptr) != 0)
        return false;

    g_response.clear();
    RKLLMInput input {};
    input.input_type = RKLLM_INPUT_PROMPT;
    input.role = "user";
    input.prompt_input = const_cast<char*>(prompt.c_str());

    RKLLMInferParam inferParam {};
    inferParam.mode = RKLLM_INFER_GENERATE;
    inferParam.keep_history = 0;
    if (rkllm_run(handle, &input, &inferParam, nullptr) != 0)
        return false;

    response = g_response;
    return true;
}

bool serveClient(int clientFd, LLMHandle handle, bool& stopping)
{
    for (std::string line; readLine(clientFd, line);) {
        if (line == "QUIT") {
            stopping = true;
            return writeLine(clientFd, "BYE");
        }

        static const std::string prefix = "REQUEST ";
        if (line.rfind(prefix, 0) != 0) {
            if (!writeLine(clientFd, "ERROR invalid request"))
                return false;
            continue;
        }

        std::string prompt;
        if (!base64Decode(line.substr(prefix.size()), prompt)) {
            if (!writeLine(clientFd, "ERROR invalid base64"))
                return false;
            continue;
        }

        std::string response;
        if (!runPrompt(handle, prompt, response)) {
            if (!writeLine(clientFd, "ERROR inference failed"))
                return false;
            continue;
        }

        if (!writeLine(clientFd, "RESPONSE " + base64Encode(response)))
            return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 3 || argc > 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <model.rkllm> <unix_socket> [max_new_tokens] [temperature]" << std::endl;
        return 1;
    }

    const int maxNewTokens = argc >= 4 ? std::atoi(argv[3]) : 256;
    const float temperature = argc >= 5 ? std::strtof(argv[4], nullptr) : 0.2F;
    if (maxNewTokens <= 0 || temperature < 0.0F) {
        std::cerr << "Invalid inference parameters" << std::endl;
        return 1;
    }

    RKLLMParam param = rkllm_createDefaultParam();
    param.model_path = argv[1];
    param.top_k = 1;
    param.top_p = 0.95F;
    param.temperature = temperature;
    param.repeat_penalty = 1.1F;
    param.max_new_tokens = maxNewTokens;
    param.max_context_len = 2048;
    param.skip_special_token = true;
    param.extend_param.base_domain_id = 0;
    param.extend_param.embed_flash = 1;

    RKLLMCallback callback {};
    callback.result_callback = onResult;
    LLMHandle handle = nullptr;
    if (rkllm_init(&handle, &param, &callback) != 0) {
        std::cerr << "rkllm_init failed" << std::endl;
        return 1;
    }

    const char* socketPath = argv[2];
    if (std::strlen(socketPath) >= sizeof(sockaddr_un::sun_path)) {
        std::cerr << "Socket path is too long" << std::endl;
        rkllm_destroy(handle);
        return 1;
    }

    unlink(socketPath);
    const int serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverFd < 0) {
        std::perror("socket");
        rkllm_destroy(handle);
        return 1;
    }

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socketPath, sizeof(address.sun_path) - 1);
    if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(serverFd, 1) != 0) {
        std::perror("bind/listen");
        close(serverFd);
        unlink(socketPath);
        rkllm_destroy(handle);
        return 1;
    }
    chmod(socketPath, S_IRUSR | S_IWUSR);

    bool stopping = false;
    while (!stopping) {
        const int clientFd = accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        serveClient(clientFd, handle, stopping);
        close(clientFd);
    }

    close(serverFd);
    unlink(socketPath);
    rkllm_destroy(handle);
    return 0;
}
