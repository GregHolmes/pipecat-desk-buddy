"""Tools Desk Buddy can call. Plain functions: Pipecat builds the schema from
the signature and docstring."""

import os
from datetime import datetime
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

import aiohttp
import anthropic
from loguru import logger

from pipecat.services.llm_service import FunctionCallParams

WEATHER_CODES = {
    0: "clear sky",
    1: "mainly clear",
    2: "partly cloudy",
    3: "overcast",
    45: "fog",
    48: "freezing fog",
    51: "light drizzle",
    53: "drizzle",
    55: "heavy drizzle",
    61: "light rain",
    63: "rain",
    65: "heavy rain",
    66: "freezing rain",
    67: "heavy freezing rain",
    71: "light snow",
    73: "snow",
    75: "heavy snow",
    77: "snow grains",
    80: "light showers",
    81: "showers",
    82: "heavy showers",
    85: "snow showers",
    86: "heavy snow showers",
    95: "thunderstorms",
    96: "thunderstorms with hail",
    99: "thunderstorms with heavy hail",
}

TIMEOUT = aiohttp.ClientTimeout(total=8)


async def _geocode(session: aiohttp.ClientSession, location: str) -> dict | None:
    # Open-Meteo matches place names only, so search on the first part and
    # use the rest ("Tipperary, Ireland") to pick between same-named places.
    name, _, hint = location.partition(",")
    async with session.get(
        "https://geocoding-api.open-meteo.com/v1/search",
        params={"name": name.strip(), "count": 10, "language": "en"},
    ) as resp:
        results = (await resp.json()).get("results") or []
    if not results:
        return None
    hint = hint.strip().lower()
    if hint:
        for r in results:
            fields = [r.get("country", ""), r.get("country_code", ""), r.get("admin1", "")]
            if any(hint in f.lower() for f in fields if f):
                return r
    return results[0]


async def get_weather(params: FunctionCallParams, location: str):
    """Get the current weather and today's forecast for a place.

    Args:
        location: A town or city, optionally with region or country, e.g. "Tipperary, Ireland".
    """
    try:
        async with aiohttp.ClientSession(timeout=TIMEOUT) as session:
            place = await _geocode(session, location)
            if not place:
                await params.result_callback({"error": f"Couldn't find a place called {location}."})
                return
            async with session.get(
                "https://api.open-meteo.com/v1/forecast",
                params={
                    "latitude": place["latitude"],
                    "longitude": place["longitude"],
                    "current": "temperature_2m,apparent_temperature,weather_code,wind_speed_10m",
                    "daily": "temperature_2m_max,temperature_2m_min,precipitation_probability_max",
                    "forecast_days": 1,
                    "timezone": "auto",
                },
            ) as resp:
                data = await resp.json()
    except Exception as e:
        logger.warning(f"Weather lookup failed: {e}")
        await params.result_callback({"error": "The weather service didn't respond."})
        return

    current, daily = data["current"], data["daily"]
    result = {
        "place": ", ".join(p for p in [place.get("name"), place.get("admin1"), place.get("country")] if p),
        "conditions": WEATHER_CODES.get(current["weather_code"], "unknown"),
        "temperature_c": round(current["temperature_2m"]),
        "feels_like_c": round(current["apparent_temperature"]),
        "wind_kmh": round(current["wind_speed_10m"]),
        "today_high_c": round(daily["temperature_2m_max"][0]),
        "today_low_c": round(daily["temperature_2m_min"][0]),
        "chance_of_rain_percent": daily["precipitation_probability_max"][0],
    }
    logger.info(f"Weather: {result}")
    await params.result_callback(result)


async def get_current_time(params: FunctionCallParams, timezone: str = "Europe/London"):
    """Get the current date and time.

    Args:
        timezone: IANA timezone name, e.g. "Europe/London" or "America/New_York". Defaults to the user's (UK) time.
    """
    try:
        tz = ZoneInfo(timezone)
    except ZoneInfoNotFoundError:
        await params.result_callback({"error": f"Unknown timezone {timezone}."})
        return
    now = datetime.now(tz)
    await params.result_callback(
        {"timezone": timezone, "time": now.strftime("%H:%M"), "date": now.strftime("%A %d %B %Y")}
    )


_anthropic = None
SEARCH_MODEL = os.getenv("DESK_BUDDY_SEARCH_MODEL", "claude-haiku-5-5")


async def search_web(params: FunctionCallParams, query: str):
    """Search the internet for current information: news, facts, prices, sports
    results, events, people, anything that may have changed recently.

    Args:
        query: What to look up, as a clear question or search phrase.
    """
    # Pipecat's Anthropic service can't pass Anthropic's server-side web search
    # tool through, so this tool makes its own call with web search enabled.
    global _anthropic
    if _anthropic is None:
        _anthropic = anthropic.AsyncAnthropic()
    try:
        response = await _anthropic.messages.create(
            model=SEARCH_MODEL,
            max_tokens=500,
            tools=[{"type": "web_search_20250305", "name": "web_search", "max_uses": 3}],
            messages=[
                {
                    "role": "user",
                    "content": (
                        f"Search the web and answer: {query}\n\n"
                        "Reply with only the key facts in two or three plain sentences, "
                        "no markdown, no links, no citations. Include dates where relevant."
                    ),
                }
            ],
        )
    except Exception as e:
        logger.warning(f"Web search failed: {e}")
        await params.result_callback({"error": "The web search didn't work just now."})
        return
    answer = "".join(b.text for b in response.content if b.type == "text").strip()
    logger.info(f"Web search [{query}]: {answer}")
    await params.result_callback({"query": query, "answer": answer})


TOOLS = [get_weather, get_current_time, search_web]
