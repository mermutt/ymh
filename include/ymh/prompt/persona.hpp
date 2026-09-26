#pragma once

// Persona and the default prompt registration (36 §2.3). Deployment persona is
// two global sections: `deployment:persona-prefix` (order 0, `complete` iff
// configured) and `deployment:persona-suffix` (order 10200). `PersonaConfig::
// prefix` is required; an explicitly empty prefix fails load (36-I5). The
// default registration also wires `harness:identity` and the session-invariant
// `{{model}}` / `{{cwd}}` variable providers.

#include <string>

#include "ymh/prompt/system_prompt.hpp"

namespace ymh {

struct PersonaConfig {
    std::string prefix;
    std::string suffix;
    bool        complete = false;
    bool        include_runtime_context = true;
};

struct PersonaHandles {
    SectionHandle prefix;
    SectionHandle suffix;
};

// The shipped `standard` preset persona (36 §2.3): prefix uses `{{model}}`,
// suffix uses `{{cwd}}`, `complete` false, runtime context included.
[[nodiscard]] PersonaConfig default_persona_config();

[[nodiscard]] PersonaHandles register_persona(SystemPrompt& prompt, const PersonaConfig& persona);

struct DefaultPromptConfig {
    std::string   identity;
    PersonaConfig persona;
    std::string   model;
    std::string   cwd;
};

struct DefaultPromptHandles {
    SectionHandle  identity;
    PersonaHandles persona;
    VariableHandle model;
    VariableHandle cwd;
};

[[nodiscard]] DefaultPromptHandles register_default_prompt(SystemPrompt& prompt,
                                                           DefaultPromptConfig config);

} // namespace ymh
