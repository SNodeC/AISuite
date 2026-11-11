#include <ai/openai/codex/protocol/JsonLineFramer.h>

int main() {
    ai::openai::codex::protocol::JsonLineFramer framer(1024);
    bool received = false;
    bool failed = false;
    const auto encoded = ai::openai::codex::protocol::JsonLineFramer::encode({{"installed", true}}, 1024);
    framer.consume(
        encoded,
        [&received](const nlohmann::json& message) {
            received = message.at("installed").get<bool>();
        },
        [&failed](const std::string&) {
            failed = true;
        });
    return received && !failed ? 0 : 1;
}
