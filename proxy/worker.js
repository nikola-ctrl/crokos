// CrokPocket -> Gemini API: прокси на Cloudflare Workers.
// Консоль шлёт запрос сюда, воркер пересылает его в Google со своего (не российского) адреса.
// Пропускает только запросы к Gemini API, ключ передаётся от консоли в заголовке x-goog-api-key.

const UPSTREAM = "https://generativelanguage.googleapis.com";

export default {
  async fetch(request) {
    const url = new URL(request.url);

    // Простая проверка, что воркер жив: открыть адрес в браузере
    if (!url.pathname.startsWith("/v1beta/") && !url.pathname.startsWith("/v1/")) {
      return new Response("CrokPocket Gemini proxy OK", { status: 200 });
    }
    if (request.method !== "POST" && request.method !== "GET") {
      return new Response("Method not allowed", { status: 405 });
    }

    const headers = new Headers();
    headers.set("content-type", "application/json");
    const key = request.headers.get("x-goog-api-key");
    if (key) headers.set("x-goog-api-key", key);

    const upstream = await fetch(UPSTREAM + url.pathname + url.search, {
      method: request.method,
      headers,
      body: request.method === "POST" ? await request.arrayBuffer() : undefined,
    });

    return new Response(upstream.body, {
      status: upstream.status,
      headers: { "content-type": upstream.headers.get("content-type") || "application/json" },
    });
  },
};
