# Repository guidance for coding assistants

## Language policy

- Write project documentation in English.
- Write code comments in English.
- Write texts, messages, helps in code in English.
- Write Git commit subjects and bodies in English.
- Communicate with the user in their preferred natural language, including progress updates, questions, and final responses.

Determine the conversation language in this order:

1. Follow an explicit language request from the user.
2. When continuing or resuming a session, preserve the language established in that conversation, including language preferences recorded in its continuation summary. English repository files or an English summary do not by themselves change the conversation language.
3. For a new conversation, infer the language from the user's opening words and messages. Do not treat quoted text, code, or technical terms as a language switch.
4. If the current conversation provides no clear signal, use a known language preference from other sessions when that context is available. Do not assume access to unavailable session history.
5. If no preference can be inferred, ask briefly which language the user prefers.

The conversation language does not change the English-language requirements for tracked documentation, code comments, or Git commit messages.

## Working rules

- AI assistants may create local Git commits, but must never run `git push` or use another tool to push commits or branches to a remote repository. A human performs all pushes.
