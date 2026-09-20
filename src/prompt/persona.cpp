#include "ymh/prompt/persona.hpp"

#include <optional>
#include <string>
#include <utility>

#include "ymh/config/config.hpp"
#include "ymh/prompt/order.hpp"

namespace ymh {

PersonaConfig default_persona_config() {
    PersonaConfig persona;
    persona.prefix = "You are a coding agent powered by the {{model}} model.";
    persona.suffix = "Your working directory is {{cwd}}.";
    return persona;
}

PersonaHandles register_persona(SystemPrompt& prompt, const PersonaConfig& persona) {
    if (persona.prefix.empty()) {
        throw ConfigError("prompt.persona.prefix must be non-empty");
    }

    PersonaHandles handles;

    PromptSection prefix;
    prefix.name     = "deployment:persona-prefix";
    prefix.order    = section_order("deployment:persona-prefix");
    prefix.text     = [text = persona.prefix](const AssembleContext&) { return text; };
    prefix.complete = persona.complete;
    handles.prefix  = prompt.section(std::move(prefix));

    PromptSection suffix;
    suffix.name    = "deployment:persona-suffix";
    suffix.order   = section_order("deployment:persona-suffix");
    suffix.text    = [text = persona.suffix](const AssembleContext&) { return text; };
    handles.suffix = prompt.section(std::move(suffix));

    return handles;
}

DefaultPromptHandles register_default_prompt(SystemPrompt& prompt, DefaultPromptConfig config) {
    DefaultPromptHandles handles;

    PromptSection identity;
    identity.name  = "harness:identity";
    identity.order = section_order("harness:identity");
    identity.text  = [text = std::move(config.identity)](const AssembleContext&) { return text; };
    handles.identity = prompt.section(std::move(identity));

    handles.persona = register_persona(prompt, config.persona);

    handles.model = prompt.variable("model", [model = std::move(config.model)](
                                                  const AssembleContext&) {
        return std::optional<std::string>{model};
    });
    handles.cwd = prompt.variable(
        "cwd", [cwd = std::move(config.cwd)](const AssembleContext&) {
            return std::optional<std::string>{cwd};
        });

    return handles;
}

} // namespace ymh
