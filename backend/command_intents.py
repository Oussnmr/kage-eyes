"""Fast, deterministic voice-command recognition shared by server and PC client.

Only supported actions live here. A phrase that does not clearly match stays a
normal conversation request; in particular, questions about a command do not
execute it.
"""

import re
import unicodedata


def normalize(text):
    text = unicodedata.normalize("NFKD", text.lower())
    text = "".join(char for char in text if not unicodedata.combining(char))
    return " ".join(re.findall(r"[a-z0-9]+", text))


def has(text, phrase):
    return f" {normalize(phrase)} " in f" {text} "


def clean_request(text):
    text = normalize(text)
    # Common wake words and polite framing do not change the intent.
    text = re.sub(r"^(?:hey |ok |okay )?(?:kage |cage )", "", text)
    text = re.sub(r"^(?:s il te plait |stp |svp |please |can you |could you |wil je |kun je |kan je |alsjeblieft )", "", text)
    text = re.sub(r" (?:s il te plait|stp|svp|please|alsjeblieft)$", "", text)
    text = text.strip()
    # Stable phonetic STT confusions observed on the live M920q microphone.
    # Keep these as exact rewrites so generic conversation is not affected.
    transcript_corrections = {
        "torn off de disc": "turn off the desk",
        "turn off de disc": "turn off the desk",
        "torn off the disc": "turn off the desk",
        "etrelle ledez": "eteins les leds",
        "etrelle les ledez": "eteins les leds",
        "je m envoie": "en avant",
    }
    return transcript_corrections.get(text, text)


ROBOT_PHRASES = {
    "idle": (
        "stop", "be normal", "return to normal", "go back to normal", "calm down",
        "relax", "reset yourself", "return to idle", "go idle", "normal mode",
        "back to idle", "arrete", "reviens a la normale", "retourne a la normale",
        "redeviens normal", "redeviens normale", "calme toi", "reviens normal",
        "retourne a ton etat normal", "mets toi en mode normal", "au repos",
    ),
    "blink": (
        "blink", "blink your eyes", "blink twice", "close and open your eyes",
        "cligne des yeux", "cligne", "clignote", "cligne deux fois",
    ),
    "sleep": (
        "go to sleep", "sleep", "sleep now", "enter sleep mode", "take a nap",
        "rest", "go back to sleep", "endors toi", "va dormir", "retourne dormir",
        "rendort toi", "mets toi en veille", "passe en veille", "mode veille",
    ),
    "angry": (
        "be angry", "get angry", "act angry", "look angry", "angry mode",
        "mets toi en colere", "sois en colere", "fais semblant d etre en colere",
        "mode colere", "en colere",
    ),
    "dizzy": (
        "spin", "spin around", "get dizzy", "act dizzy", "dizzy mode",
        "tourne sur toi meme", "etourdis toi", "mets toi en mode etourdi",
    ),
    "360": (
        "360", "three sixty", "three hundred sixty", "three hundred and sixty",
        "trois cent soixante", "tourne a 360", "tourne a trois cent soixante",
        "fais un 360", "fais trois cent soixante", "tourne sur toi meme a 360",
        "tourne sur toi meme a trois cent soixante",
    ),
    "dance": (
        "dance", "dance for me", "do a dance", "dance on the spot", "please dance",
        "danse", "danse pour moi", "fais une danse", "fais moi une danse",
        "danse sur place", "fais quelques pas",
    ),
    "explore": (
        "explore", "look around", "scan around", "explore around", "look around the room",
        "explore la pièce", "explore autour", "regarde autour", "regarde autour de toi",
        "inspecte autour",
    ),
    # Deliberately short, exact bench-test commands.  They are matched as a
    # complete request below, so ordinary sentences containing "move" cannot
    # accidentally energize an actuator.
    "move_h": ("move h", "move horizontal", "horizontal", "horizontale"),
    "move_v": ("move v", "move vertical", "vertical", "verticale"),
    "move_f": ("move f", "move forward", "en avant"),
    "move_b": ("move b", "move back", "move backward", "en arriere"),
}

BACKEND_PHRASES = {
    "ollama": (
        "switch to local", "use local", "go local", "local mode", "use ollama",
        "switch to ollama", "go back to local", "passe en local", "mode local",
        "utilise le mode local", "repasse en local", "utilise ollama",
    ),
    "codex": (
        "switch to chatgpt", "switch to codex", "use chatgpt", "use codex",
        "back to chatgpt", "cloud mode", "passe sur chatgpt", "repasse sur chatgpt",
        "utilise chatgpt", "mode chatgpt", "passe sur codex", "utilise codex",
    ),
}

LANGUAGE_PHRASES = {
    "en": (
        "reponds en anglais", "repond en anglais", "parle en anglais", "parle anglais",
        "passe en anglais", "mode anglais", "en anglais", "answer in english", "reply in english",
        "speak english", "switch to english", "english mode",
    ),
    "fr": (
        "reponds en francais", "repond en francais", "parle en francais", "parle francais",
        "passe en francais", "mode francais", "en francais", "answer in french", "reply in french",
        "speak french", "switch to french", "french mode",
    ),
}

SESSION_END_PHRASES = (
    "stop listening", "stop talking", "end the conversation", "end chat",
    "be quiet", "that s all", "goodbye", "bye for now", "go back to sleep",
    "stop the chat", "close the shop", "close everything down",
    "arrete de parler", "arrete d ecouter", "termine la conversation",
    "fin de conversation", "c est tout", "au revoir", "tais toi",
    "retourne dormir", "va dormir", "rendort toi", "reviens plus tard",
)

SHUTDOWN_PHRASES = ("close the shop", "close everything", "close everything down")

PROFILE_PHRASES = {
    "nightshift": (
        "nightshift", "night shift", "night mode", "mode nuit", "mode nocturne",
        "active le mode nuit", "passe en mode nuit", "mets night shift",
    ),
    "rest": (
        "rest mode", "restmode", "rest time", "mode repos", "mode detente",
        "active le mode repos", "passe en mode repos",
    ),
}

DEVICE_ALIASES = {
    "light": ("light", "lights", "main light", "ceiling light", "ceiling",
              "lumiere", "lumieres", "plafonnier", "plafonier", "eclairage"),
    "lamp": ("lamp", "desk lamp", "bedside lamp", "lampe"),
    "projector": ("projector", "beamer", "projecteur", "videoprojecteur"),
    "leds": ("led", "leds", "ledes", "ledez", "led light", "led lights",
             "lumiere led", "lumieres led", "bande led", "ruban led"),
    "desk": ("desk power", "desk plug", "power strip",
             "multiprise", "multiprises",
             "prise du bureau", "prise de bureau"),
}

ON_PHRASES = (
    "turn on", "switch on", "power on", "enable", "allume", "allumer",
    "active", "activer", "mets en marche", "mets", "marche", "ouvre", "lume", "lumes",
)
OFF_PHRASES = (
    "turn off", "switch off", "power off", "shut off", "disable", "kill",
    "eteins", "eteint", "eteind", "eteindre", "etant", "coupe", "couper",
    "desactive", "desactiver",
    "ferme", "fermer", "arrete", "stoppe", "mets hors tension",
)
ALL_PHRASES = ("everything", "all devices", "all the lights", "all lights",
               "tout", "tous", "toutes les lumieres")


def _complete_match(text, phrases):
    return any(text == normalize(phrase) or text.endswith(" " + normalize(phrase))
               for phrase in phrases)


def session_intent(text):
    text = clean_request(text)
    if _complete_match(text, SHUTDOWN_PHRASES):
        return "shutdown"
    if _complete_match(text, SESSION_END_PHRASES):
        return "end"
    return None


def direct_intent(text):
    text = clean_request(text)
    for language, phrases in LANGUAGE_PHRASES.items():
        if _complete_match(text, phrases):
            return "language", language
    for backend, phrases in BACKEND_PHRASES.items():
        if _complete_match(text, phrases):
            return "backend", backend
    for action, phrases in ROBOT_PHRASES.items():
        if action in {"dance", "explore"} and text.startswith((
            "what is ", "what does ", "why ", "how ", "tell me about ",
            "qu est ce que ", "pourquoi ", "comment ", "explique ",
        )):
            continue
        if _complete_match(text, phrases):
            return "robot", action
    return None


def home_intent(text):
    text = clean_request(text)
    # A question about the setting is not an instruction to change it.
    if any(text.startswith(prefix) for prefix in (
        "what is ", "what does ", "why ", "how ", "qu est ce que ",
        "pourquoi ", "comment ", "je veux savoir ", "explique ", "tell me ",
    )):
        return None
    for profile, phrases in PROFILE_PHRASES.items():
        if _complete_match(text, phrases):
            return "profile", profile
    on = any(has(text, phrase) for phrase in ON_PHRASES)
    off = any(has(text, phrase) for phrase in OFF_PHRASES)
    # Some English forms put the object between verb and particle.
    on = on or bool(re.search(r"\b(?:turn|switch)\b.+\bon\b$", text))
    off = off or bool(re.search(r"\b(?:turn|switch)\b.+\boff\b$", text))
    if on == off:
        return None
    targets = {name for name, aliases in DEVICE_ALIASES.items()
               if any(has(text, alias) for alias in aliases)}
    # "bureau/desk" alone means its power strip, but in "lumière du
    # bureau/desk light" it is only a location qualifier.
    if not targets and re.search(r"\b(?:le |the )?(?:bureau|desk)$", text):
        targets.add("desk")
    # "led light" is one LED target, not both the LED strip and ceiling light.
    if "leds" in targets and any(has(text, phrase) for phrase in DEVICE_ALIASES["leds"]):
        without_leds = text
        for phrase in sorted(DEVICE_ALIASES["leds"], key=len, reverse=True):
            without_leds = re.sub(r"\b" + re.escape(normalize(phrase)) + r"\b", " ", without_leds)
        if not any(has(without_leds, alias) for alias in DEVICE_ALIASES["light"]):
            targets.discard("light")
    if not targets and any(has(text, phrase) for phrase in ALL_PHRASES):
        targets = set(DEVICE_ALIASES)
    return ("devices", tuple(name for name in DEVICE_ALIASES if name in targets), on) if targets else None


def is_direct(text):
    return (direct_intent(text) is not None or home_intent(text) is not None
            or looks_like_home_command(text))


def looks_like_home_command(text):
    """Catch an ambiguous device command before it reaches the LLM."""
    text = clean_request(text)
    if any(text.startswith(prefix) for prefix in (
        "what is ", "what does ", "why ", "how ", "qu est ce que ",
        "pourquoi ", "comment ", "je veux savoir ", "explique ", "tell me ",
    )):
        return False
    device_seen = any(
        has(text, alias) for aliases in DEVICE_ALIASES.values() for alias in aliases
    ) or bool(re.search(r"\b(?:bureau|desk)$", text))
    command_seen = any(has(text, phrase) for phrase in ON_PHRASES + OFF_PHRASES)
    return device_seen and command_seen
