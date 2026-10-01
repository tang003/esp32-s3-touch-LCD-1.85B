import {
  transformAmapRouteOptionsV2,
  transformAmapRouteV2,
} from "./amap-transformer.js";
import { wgs84ToGcj02 } from "./coordinates.js";
import {
  fetchGetWithTransientRetry,
  isTransientUpstreamStatus,
} from "./upstream-retry.js";

const DEFAULT_DRIVING_ENDPOINT = "https://restapi.amap.com/v5/direction/driving";
const DEFAULT_ELECTROBIKE_ENDPOINT = "https://restapi.amap.com/v5/direction/electrobike";

export class AmapProviderError extends Error {
  constructor(code, message, { retryable = true } = {}) {
    super(message);
    this.name = "AmapProviderError";
    this.code = code;
    this.retryable = retryable;
  }
}

function formatProviderPoint(point) {
  return `${Number(point.longitude_deg).toFixed(6)},${Number(point.latitude_deg).toFixed(6)}`;
}

function buildAmapRouteUrl(request, { key, endpoint }) {
  if (typeof key !== "string" || key.trim() === "") {
    throw new AmapProviderError("SERVER_MISCONFIGURED", "AMap server key is not configured", {
      retryable: false,
    });
  }

  const origin = wgs84ToGcj02(request.origin);
  const destination = wgs84ToGcj02(request.destination);
  const url = new URL(endpoint);
  if (url.protocol !== "https:") {
    throw new AmapProviderError("SERVER_MISCONFIGURED", "AMap endpoint must use HTTPS", {
      retryable: false,
    });
  }
  url.searchParams.set("key", key);
  url.searchParams.set("origin", formatProviderPoint(origin));
  url.searchParams.set("destination", formatProviderPoint(destination));
  return url;
}

export function buildAmapDrivingUrl(request, { key, endpoint = DEFAULT_DRIVING_ENDPOINT }) {
  const url = buildAmapRouteUrl(request, { key, endpoint });
  url.searchParams.set("strategy", "32");
  url.searchParams.set("show_fields", "cost,polyline,navi,tmcs");
  if (request.destination_poi_id) {
    url.searchParams.set("destination_id", request.destination_poi_id);
  }
  return url;
}

export function buildAmapElectrobikeUrl(
  request,
  { key, endpoint = DEFAULT_ELECTROBIKE_ENDPOINT },
) {
  const url = buildAmapRouteUrl(request, { key, endpoint });
  url.searchParams.set("show_fields", "cost,navi,polyline");
  url.searchParams.set("alternative_route", "3");
  return url;
}

export function createAmapProvider({
  key,
  fetchImpl = globalThis.fetch,
  endpoint = DEFAULT_DRIVING_ENDPOINT,
  electrobikeEndpoint = DEFAULT_ELECTROBIKE_ENDPOINT,
  timeoutMs = 8000,
  maxAttempts = 3,
  retryDelayMs = 100,
  clock = Date.now,
} = {}) {
  if (typeof fetchImpl !== "function") {
    throw new TypeError("fetchImpl must be a function");
  }

  async function fetchRoutePayload(request, { signal } = {}) {
      const url = request.route_mode === "electrobike"
        ? buildAmapElectrobikeUrl(request, { key, endpoint: electrobikeEndpoint })
        : buildAmapDrivingUrl(request, { key, endpoint });
      const deadlineSignal = AbortSignal.timeout(timeoutMs);
      const signals = [deadlineSignal];
      if (signal) signals.push(signal);
      const requestSignal = AbortSignal.any(signals);

      let response;
      try {
        response = await fetchGetWithTransientRetry(fetchImpl, url, {
          headers: { Accept: "application/json" },
          signal: requestSignal,
          maxAttempts,
          retryDelayMs,
        });
      } catch (error) {
        const code =
          deadlineSignal.aborted || error?.name === "TimeoutError"
            ? "PROVIDER_TIMEOUT"
            : "PROVIDER_UNAVAILABLE";
        throw new AmapProviderError(code, "AMap route service is unavailable");
      }

      if (!response.ok) {
        throw new AmapProviderError(
          "PROVIDER_HTTP_ERROR",
          `AMap returned HTTP ${response.status}`,
          {
            retryable:
              isTransientUpstreamStatus(response.status) || response.status === 429,
          },
        );
      }

      let payload;
      try {
        payload = await response.json();
      } catch {
        throw new AmapProviderError("PROVIDER_INVALID_JSON", "AMap returned invalid JSON");
      }
      return payload;
  }

  return {
    async planRoute(request, options = {}) {
      const payload = await fetchRoutePayload(request, options);
      return transformAmapRouteV2(payload, { generatedAtMs: clock() });
    },

    async planRouteOptions(request, options = {}) {
      const payload = await fetchRoutePayload(request, options);
      return transformAmapRouteOptionsV2(payload, { generatedAtMs: clock() });
    },
  };
}
