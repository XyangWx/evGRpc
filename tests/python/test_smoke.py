"""Sanity test: validates that auth + channel + 1 RPC work end-to-end."""

from tests.python.gen.evgrpc import weather_pb2 as pb
from tests.python.gen.evgrpc import weather_pb2_grpc as rpc


def test_search_weather_with_valid_bearer_succeeds(channel):
    """With valid bearer, SearchWeather (no prefix) returns 200 + response."""
    stub = rpc.WeatherServiceStub(channel)
    resp = stub.SearchWeather(pb.SearchWeatherRequest(prefix="", limit=1))
    assert isinstance(resp, pb.SearchWeatherResponse)