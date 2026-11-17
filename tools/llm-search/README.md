# LLM Search & Fetch — безопасный веб-поиск и чтение страниц для локальных моделей

> **Статус:** на lmserver развёрнута и работает **Rust-версия** — [`rust/`](rust/README.md)
> (один бинарник, `impl: "rust"` в `/health`). Python-реализация в этом каталоге
> оставлена как референс; её поисковый каскад на lmserver блокируется антиботом
> DuckDuckGo из-за TLS-отпечатка Python (`httpx` → `202`), поэтому для поиска
> используйте Rust-сборку.

Сервис на lmserver (`127.0.0.1:8099`): поиск, безопасная загрузка страниц и «ресёрч»
(поиск + чтение топ-результатов) для локальных LLM. Без Docker, без API-ключей,
без браузера. Хранит только текстовый кэш (Python — SQLite, Rust — в памяти).

## Что умеет

| Метод | Что делает |
|---|---|
| `GET /search?q=…&count=8` | метапоиск: DuckDuckGo HTML (POST+сессия) → Bing HTML → Bing RSS → Wikipedia → (опционально) SearXNG; дедупликация |
| `GET /fetch?url=…&max_chars=8000` | загрузка страницы с SSRF-защитой, robots.txt, лимитами; извлечение читаемого текста |
| `GET /research?q=…&count=4&budget=12000` | поиск + параллельное чтение топ-N, отдаёт текст с источниками под контекст модели |
| `GET /tools.json` | схемы инструментов в формате OpenAI function calling |
| `GET /openapi.json`, `/docs` | OpenAPI (импортируется как tool в Open WebUI и совместимые клиенты) |
| `GET /health` | статус, кэш, аптайм |

`POST`-варианты тех же методов принимают JSON-тело.

## Безопасность

- Слушает **только 127.0.0.1** — снаружи недоступен.
- **SSRF-защита**: только `http/https`, только порты 80/443, DNS-резолв проверяется —
  приватные, loopback, link-local, CGNAT и прочие не-публичные адреса блокируются;
  редиректы валидируются на каждом шаге (макс. 4).
- **Лимиты**: ответ ≤ 2 МБ, таймауты 5 с connect / 12 с read, ≤ 6 параллельных загрузок.
- **robots.txt** соблюдается (кэш 1 ч), User-Agent честный.
- Только чтение: без куки, без авторизации, без выполнения JS. Никаких записей на сайты.
- Кэш — только извлечённый текст, SQLite с TTL и лимитом 64 МБ.
- systemd-юнит с `ProtectSystem=strict`, `PrivateTmp`, `NoNewPrivileges`,
  `MemoryMax=512M`, `CPUQuota=150%`, запись только в `/data/search-svc/data`.
- Опциональный токен: `SEARCH_TOKEN` в `search.env` → требуется `Authorization: Bearer <token>`.

Чего сервис **не делает** и почему: не обходит капчи и анти-бот (DDG отдаёт 202,
Yandex — капчу; используется Bing), не ходит во внутреннюю сеть, не рендерит JS
(для JS-страниц есть opt-in фолбэк через Kitesurf, по умолчанию выключен).

## Установка (уже выполнена на lmserver)

```
/data/search-svc/
├── app.py, config.py, cache.py, safety.py, fetch.py, search.py
├── search.env            # конфигурация
├── data/cache.sqlite3    # кэш
└── venv/                 # python 3.14 + fastapi, httpx, lxml, trafilatura
```

Юнит: `/etc/systemd/system/search-svc.service` (пользователь `searchsvc`, nologin).

```bash
systemctl status search-svc
journalctl -u search-svc -f
```

## Проверка

```bash
curl -s 'http://127.0.0.1:8099/health'
curl -s 'http://127.0.0.1:8099/search?q=pearl+mining+pool&count=5'
curl -s 'http://127.0.0.1:8099/fetch?url=https://example.com/&max_chars=2000'
curl -s 'http://127.0.0.1:8099/research?q=rtx+3090+pearlhash+settings&count=3'
```

## Подключение к локальной модели

### Ollama (tool calling, Python)

```python
import json, requests

BASE = "http://127.0.0.1:8099"
tools = list(requests.get(BASE + "/tools.json").json()["tools"].values())

def call_tool(name, args):
    if name == "search_web":
        return requests.get(BASE + "/search", params={"q": args["query"], "count": args.get("count", 8)}).json()
    if name == "fetch_url":
        return requests.get(BASE + "/fetch", params={"url": args["url"], "max_chars": args.get("max_chars", 8000)}).json()
    if name == "research":
        return requests.get(BASE + "/research", params={"q": args["query"], "count": args.get("count", 4)}).json()
    raise ValueError(name)

r = requests.post("http://127.0.0.1:11434/api/chat", json={
    "model": "qwen3:32b",
    "messages": [{"role": "user", "content": "Найди свежие настройки PearlHash для 3090 и кратко изложи."}],
    "tools": tools,
    "stream": False,
})
msg = r.json()["message"]
for call in msg.get("tool_calls", []):
    out = call_tool(call["function"]["name"], call["function"]["arguments"])
    # ... отправить результат обратно в модель вторым запросом
```

### Open WebUI

`Настройки → Инструменты → OpenAPI` — импортировать `http://127.0.0.1:8099/openapi.json`
(сервер Open WebUI должен видеть lmserver по сети; тогда укажите адрес сервиса,
например `http://192.168.1.215:8099`, и включите `SEARCH_TOKEN`).

### Любой OpenAI-совместимый клиент

Инструменты: `GET /tools.json`. Все эндпоинты также принимают `POST` с JSON.

## Конфигурация (`search.env`)

| Переменная | По умолчанию | Смысл |
|---|---|---|
| `SEARCH_TOKEN` | пусто | Bearer-токен (пусто = без авторизации, только localhost) |
| `SEARCH_SEARX_URL` | пусто | адрес своего SearXNG с включённым JSON API |
| `SEARCH_KITESURF_FALLBACK` | `0` | opt-in рендер JS-страниц через kitesurf.dev (URL уходит в Cloudflare) |
| `SEARCH_CACHE_MB` | 64 | лимит кэша |
| `SEARCH_RATE_PER_MIN` | 60 | лимит запросов на IP в минуту |
| `SEARCH_MAX_BYTES` | 2 МБ | максимум на страницу |
| `SEARCH_RESPECT_ROBOTS` | 1 | соблюдать robots.txt |
| `SEARCH_FETCH_TTL` | 21600 | TTL кэша страниц, сек |

## Почему так, а не Kitesurf/SearXNG «из коробки»

- Kitesurf — движок браузера, а не поиск; здесь он оставлен опциональным фолбэком
  для JS-страниц (по умолчанию выключен, чтобы URL не уходили наружу).
- SearXNG без Docker требует своего стека и диска; на lmserver свободно ~14 ГБ,
  поэтому базовая схема — внешние HTML-эндпоинты (DDG/Bing) с фолбэком на Bing RSS
  и Wikipedia. SearXNG подключается одной переменной, если понадобится.
- Тяжёлые зависимости не ставились: итого venv 67 МБ, RAM сервиса ~60–90 МБ.
- Антибот по TLS-отпечатку: у Python (`httpx`) DDG отвечает `202`, у Rust/native-tls
  и curl — `200`. Поэтому на lmserver работает Rust-версия.
