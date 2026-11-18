"""Desk Buddy: Pipecat bot for an M5Stack CoreS3 with an animated face.

Deepgram Flux STT -> Anthropic -> Deepgram Aura-2 TTS, over SmallWebRTC.

Face events reach the device as RTVI messages. Pipecat already sends
user/bot started/stopped speaking; Flux eager end of turn and turn resumed
are sent as ``server-message`` with ``{"event": ...}``.

Run with the device:  uv run bot.py -t webrtc --esp32 --host <mac-ip>
Run in a browser:     uv run bot.py
"""

import os

from dotenv import load_dotenv
from loguru import logger

from pipecat.frames.frames import LLMRunFrame, TTSSpeakFrame
from pipecat.pipeline.pipeline import Pipeline
from pipecat.pipeline.worker import PipelineParams, PipelineWorker, ProcessorUnusablePolicy
from pipecat.processors.aggregators.llm_response_universal import (
    AssistantTurnStoppedMessage,
    LLMContext,
    LLMContextAggregatorPair,
    LLMUserAggregatorParams,
    UserTurnStoppedMessage,
)
from pipecat.processors.frameworks.rtvi import RTVIObserverParams, RTVIServerMessageFrame
from pipecat.processors.frameworks.rtvi.observer import RTVIFunctionCallReportLevel
from pipecat.runner.types import RunnerArguments
from pipecat.runner.utils import create_transport
from pipecat.services.anthropic.llm import AnthropicLLMService
from pipecat.services.deepgram.flux.stt import DeepgramFluxSTTService
from pipecat.services.deepgram.tts import DeepgramTTSService
from pipecat.transports.base_transport import BaseTransport, TransportParams
from pipecat.turns.user_turn_strategies import (
    FilterIncompleteUserTurnStrategies,
    MergeMode,
    UserTurnStrategiesParams,
)
from pipecat.workers.runner import WorkerRunner

from tools import TOOLS

load_dotenv(override=True)

SYSTEM_PROMPT = (
    "You are Desk Buddy, a small, cheerful robot that lives on a desk and talks "
    "through a tiny speaker. Your replies are spoken aloud, so never use emojis, "
    "lists or formatting. Keep every reply to one or two short sentences. "
    "Be warm, curious and a little playful. "
    "You have tools for the weather, the current time, and searching the web. "
    "Use them whenever the user asks about weather, time, news, or anything "
    "current or factual you aren't sure about; never say you can't look things "
    "up. When you need a tool, call it immediately in the same response. Never "
    "say you will look something up without actually calling the tool; your "
    "face already shows the user you're looking. "
    "The user is in the UK unless they say otherwise."
)

PRESENCE_LINES = {
    "user_left": "Oh, where have you gone?",
    "user_returned": "Oh, hello!",
}

transport_params = {
    "webrtc": lambda: TransportParams(
        audio_in_enabled=True,
        audio_out_enabled=True,
    ),
}


async def send_face_event(service, event: str):
    logger.info(f"Face event: {event}")
    await service.push_frame(RTVIServerMessageFrame(data={"event": event}))


async def run_bot(transport: BaseTransport, runner_args: RunnerArguments):
    logger.info("Starting bot")

    stt = DeepgramFluxSTTService(
        api_key=os.environ["DEEPGRAM_API_KEY"],
        enable_eager_end_of_turn=True,
        settings=DeepgramFluxSTTService.Settings(
            eager_eot_threshold=float(os.getenv("FLUX_EAGER_EOT_THRESHOLD", "0.5")),
            eot_threshold=float(os.getenv("FLUX_EOT_THRESHOLD", "0.7")),
        ),
    )

    tts = DeepgramTTSService(
        api_key=os.environ["DEEPGRAM_API_KEY"],
        settings=DeepgramTTSService.Settings(
            voice=os.getenv("AURA_VOICE", "aura-2-thalia-en"),
        ),
    )

    llm = AnthropicLLMService(
        api_key=os.environ["ANTHROPIC_API_KEY"],
        settings=AnthropicLLMService.Settings(
            model=os.getenv("DESK_BUDDY_LLM_MODEL", "claude-haiku-5-5"),
            system_instruction=SYSTEM_PROMPT,
        ),
    )

    # Flux is confident "I was wondering." is a complete sentence, so on its
    # own it ends the turn there. The LLM turn-completion gate holds turns
    # the LLM judges incomplete, so the bot waits while the face shows
    # thinking, then "oh, go on" when the user carries on.
    context = LLMContext(tools=TOOLS)
    user_aggregator, assistant_aggregator = LLMContextAggregatorPair(
        context,
        user_params=LLMUserAggregatorParams(
            user_turn_strategies=FilterIncompleteUserTurnStrategies(),
            user_turn_strategies_params=UserTurnStrategiesParams(
                start_merge_mode=MergeMode.EXTEND,
                stop_merge_mode=MergeMode.EXTEND,
            ),
        ),
    )

    pipeline = Pipeline(
        [
            transport.input(),
            stt,
            user_aggregator,
            llm,
            tts,
            transport.output(),
            assistant_aggregator,
        ]
    )

    worker = PipelineWorker(
        pipeline,
        params=PipelineParams(
            enable_metrics=True,
            enable_usage_metrics=True,
        ),
        # Keep data channel traffic small: the device parses every message.
        rtvi_observer_params=RTVIObserverParams(
            bot_output_enabled=False,
            bot_llm_enabled=False,
            user_llm_enabled=False,
            user_transcription_enabled=False,
            user_input_enabled=False,
            user_mute_enabled=False,
            metrics_enabled=False,
            # Names only (no arguments/results): the face shows which tool runs.
            function_call_report_level={"*": RTVIFunctionCallReportLevel.NAME},
        ),
        idle_timeout_secs=runner_args.pipeline_idle_timeout_secs,
        processor_unusable_policy=ProcessorUnusablePolicy.END,
    )

    runner = WorkerRunner(handle_sigint=runner_args.handle_sigint)

    await runner.add_workers(worker)

    # The device is half duplex (mic off while it plays), so it can't be
    # interrupted by voice. Tapping the screen sends an RTVI client-message.
    @worker.rtvi.event_handler("on_client_message")
    async def on_client_message(rtvi, msg):
        if msg.type == "interrupt":
            logger.info("Device tapped: interrupting the bot")
            await rtvi.interrupt_bot()
        elif msg.type in PRESENCE_LINES:
            # The device's camera saw the user leave / come back.
            logger.info(f"Presence: {msg.type}")
            await worker.queue_frames([TTSSpeakFrame(PRESENCE_LINES[msg.type])])
        else:
            logger.info(f"Client message: {msg.type} {msg.data}")

    @transport.event_handler("on_client_connected")
    async def on_client_connected(transport, client):
        logger.info("Client connected")
        context.add_message(
            {"role": "developer", "content": "Say a short hello and introduce yourself."}
        )
        await worker.queue_frames([LLMRunFrame()])

    @transport.event_handler("on_client_disconnected")
    async def on_client_disconnected(transport, client):
        logger.info("Client disconnected")
        await runner.cancel()

    @stt.event_handler("on_eager_end_of_turn")
    async def on_eager_end_of_turn(service, transcript):
        logger.info(f"Eager end of turn: [{transcript}]")
        await send_face_event(service, "eager_eot")

    @stt.event_handler("on_turn_resumed")
    async def on_turn_resumed(service):
        logger.info("Turn resumed")
        await send_face_event(service, "turn_resumed")

    # Flux committed a turn the LLM gate may hold as incomplete. If Flux starts
    # a new turn before the user turn is finalised, the user carried on.
    flux_turn_pending = False

    @stt.event_handler("on_start_of_turn")
    async def on_start_of_turn(service, transcript):
        if flux_turn_pending:
            logger.info("User carried on after an incomplete turn")
            await send_face_event(service, "turn_resumed")

    @stt.event_handler("on_end_of_turn")
    async def on_end_of_turn(service, transcript):
        nonlocal flux_turn_pending
        flux_turn_pending = True
        logger.info(f"Flux end of turn: [{transcript}]")

    @user_aggregator.event_handler("on_user_turn_stopped")
    async def on_user_turn_stopped(aggregator, strategy, message: UserTurnStoppedMessage):
        nonlocal flux_turn_pending
        flux_turn_pending = False
        logger.info(f"Transcript: user: {message.content}")

    @assistant_aggregator.event_handler("on_assistant_turn_stopped")
    async def on_assistant_turn_stopped(aggregator, message: AssistantTurnStoppedMessage):
        nonlocal flux_turn_pending
        # A held incomplete turn yields an empty assistant turn; only a real
        # reply (including a "still there?" nudge) ends the pending turn.
        if message.content and message.content.strip():
            flux_turn_pending = False
        logger.info(f"Transcript: assistant: {message.content}")

    await runner.run()


async def bot(runner_args: RunnerArguments):
    """Main bot entry point compatible with Pipecat Cloud."""
    transport = await create_transport(runner_args, transport_params)
    await run_bot(transport, runner_args)


if __name__ == "__main__":
    from pipecat.runner.run import main

    main()
