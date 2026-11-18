"""Check whether the system prompt makes Claude actually call tools (not just
say it will). Mirrors the bot: system prompt + Pipecat turn-completion
instructions + the same tool schemas."""

import asyncio
import sys

import anthropic

from pipecat.adapters.llm.services.anthropic_adapter import AnthropicLLMAdapter
from pipecat.processors.aggregators.llm_response_universal import LLMContext
from pipecat.turns.user_turn_completion_mixin import USER_TURN_COMPLETION_INSTRUCTIONS

import bot
from tools import TOOLS

EXTRA = {"output_config": {"effort": sys.argv[2]}} if len(sys.argv) > 2 else {}

HISTORY = [
    {"role": "user", "content": "Say a short hello and introduce yourself."},
    {"role": "assistant", "content": "● Hello there, I'm Desk Buddy, a small cheerful robot who lives on your desk. I can check the weather, tell you the time, or look things up for you."},
]

QUESTIONS = [
    "Hello. Can you tell me what the last Arsenal Football Club results were?",
    "What's the latest news about Deepgram?",
    "What's the weather like in Tipperary?",
]


async def main(runs: int):
    client = anthropic.AsyncAnthropic()
    tools = AnthropicLLMAdapter().to_provider_tools_format(LLMContext(tools=TOOLS).tools)
    system = bot.SYSTEM_PROMPT + "\n\n" + USER_TURN_COMPLETION_INSTRUCTIONS
    for q in QUESTIONS:
        for _ in range(runs):
            r = await client.messages.create(
                model="claude-haiku-5-5",
                max_tokens=200,
                extra_body=EXTRA,
                system=system,
                tools=tools,
                messages=HISTORY + [{"role": "user", "content": q}],
            )
            kinds = [b.type if b.type != "tool_use" else f"tool_use:{b.name}" for b in r.content]
            text = "".join(b.text for b in r.content if b.type == "text").strip()
            print(f"{r.stop_reason:9} {kinds} {text[:60]!r}  <- {q[:40]}")


asyncio.run(main(int(sys.argv[1]) if len(sys.argv) > 1 else 2))
