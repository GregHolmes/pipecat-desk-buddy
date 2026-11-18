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

from pipecat.frames.frames import LLMRunFrame
from pipecat.pipeline.pipeline import Pipeline
from pipecat.pipeline.worker import PipelineParams, PipelineWorker, ProcessorUnusablePolicy
from pipecat.processors.aggregators.llm_response_universal import (
    AssistantTurnStoppedMessage,
    LLMContext,
    LLMContextAggregatorPair,
    UserTurnStoppedMessage,
)
from pipecat.processors.frameworks.rtvi import RTVIObserverParams, RTVIServerMessageFrame
from pipecat.runner.types import RunnerArguments
from pipecat.runner.utils import create_transport
from pipecat.services.anthropic.llm import AnthropicLLMService
from pipecat.services.deepgram.flux.stt import DeepgramFluxSTTService
from pipecat.services.deepgram.tts import DeepgramTTSService
from pipecat.transports.base_transport import BaseTransport, TransportParams
from pipecat.workers.runner import WorkerRunner

load_dotenv(override=True)

SYSTEM_PROMPT = (
    "You are Desk Buddy, a small, cheerful robot that lives on a desk and talks "
    "through a tiny speaker. Your replies are spoken aloud, so never use emojis, "
    "lists or formatting. Keep every reply to one or two short sentences. "
    "Be warm, curious and a little playful."
)

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
            model=os.getenv("ANTHROPIC_MODEL", "claude-haiku-5-5"),
            system_instruction=SYSTEM_PROMPT,
        ),
    )

    context = LLMContext()
    user_aggregator, assistant_aggregator = LLMContextAggregatorPair(context)

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
        ),
        idle_timeout_secs=runner_args.pipeline_idle_timeout_secs,
        processor_unusable_policy=ProcessorUnusablePolicy.END,
    )

    runner = WorkerRunner(handle_sigint=runner_args.handle_sigint)

    await runner.add_workers(worker)

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

    @stt.event_handler("on_end_of_turn")
    async def on_end_of_turn(service, transcript):
        logger.info(f"Flux end of turn: [{transcript}]")

    @user_aggregator.event_handler("on_user_turn_stopped")
    async def on_user_turn_stopped(aggregator, strategy, message: UserTurnStoppedMessage):
        logger.info(f"Transcript: user: {message.content}")

    @assistant_aggregator.event_handler("on_assistant_turn_stopped")
    async def on_assistant_turn_stopped(aggregator, message: AssistantTurnStoppedMessage):
        logger.info(f"Transcript: assistant: {message.content}")

    await runner.run()


async def bot(runner_args: RunnerArguments):
    """Main bot entry point compatible with Pipecat Cloud."""
    transport = await create_transport(runner_args, transport_params)
    await run_bot(transport, runner_args)


if __name__ == "__main__":
    from pipecat.runner.run import main

    main()
