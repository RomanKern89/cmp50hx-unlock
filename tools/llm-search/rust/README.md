# LLM Search & Fetch (Rust)

Тот же сервис, что `tools/llm-search/` (Python/FastAPI), но на Rust (axum + tokio).
Один статический бинарник, без интерпретатора, без venv, без записи на диск.

## Сборка и установка (lmserver)

```bash
# на сервере: apt install rustc cargo libssl-dev pkg-config
cd /data/build/search-rust
cargo build --release            # ~2–4 мин на 36 ядрах

sudo mkdir -p /data/search-svc-rust
sudo install -m 0755 target/release/search-svc /data/search-svc-rust/search-svc
sudo install -m 0600 search.env /data/search-svc-rust/search.env
sudo install -m 0644 search-svc.service /etc/systemd/system/search-svc.service
sudo systemctl daemon-reload && sudo systemctl restart search-svc
```

Проверка:

```bash
curl -s http://127.0.0.1:8099/health
curl -s 'http://127.0.0.1:8099/search?q=pearl+mining+pool&count=5'
curl -s 'http://127.0.0.1:8099/fetch?url=https://example.com/&max_chars=2000'
curl -s 'http://127.0.0.1:8099/research?q=rtx+3090+pearlhash+settings&count=3'
```

Тесты: `cargo test` (проверки SSRF-фильтра и robots.txt).

## API (полностью совпадает с Python-версией)

| Метод | Что делает |
|---|---|
| `GET /search?q=…&count=8` | метапоиск: Bing HTML (с декодированием `ck/a`-редиректов) → Bing RSS → Wikipedia → (опц.) SearXNG, дедупликация |
| `GET /fetch?url=…&max_chars=8000` | безопасная загрузка страницы + читаемый текст |
| `GET /research?q=…&count=4&budget=12000` | поиск + параллельное чтение топ-N |
| `GET /tools.json` | схемы инструментов в формате OpenAI function calling |
| `GET /openapi.json` | минимальный OpenAPI (для клиентов, которые его импортируют) |
| `GET /health` | статус, кэш, аптайм, `impl: "rust"` |

`POST`-варианты принимают JSON-тело. Если задан `SEARCH_TOKEN` — нужен
`Authorization: Bearer <token>`.

## Отличия от Python-версии

| | Python | Rust |
|---|---|---|
| рантайм | venv 67 МБ, Python 3.14 | один бинарник (~4–6 МБ), без зависимостей |
| память | ~60–90 МБ + SQLite | ~10–20 МБ, кэш в RAM |
| кэш | SQLite на диске (64 МБ) | в памяти с тем же TTL и лимитом (теряется при рестарте) |
| SSRF | проверка DNS, затем обычный запрос | **DNS-пиннинг**: соединение идёт на проверенный IP (закрыт DNS-rebinding) |
| извлечение | trafilatura | свой обход DOM (scraper/html5ever) + приоритет `article/main` |
| `/docs` | Swagger UI | нет (есть `/openapi.json` и `/tools.json`) |

Общее: только `127.0.0.1`, только `http/https` и порты 80/443, блокировка
приватных/loopback/link-local/CGNAT адресов, robots.txt, лимит 2 МБ на страницу,
≤6 параллельных загрузок, редиректы проверяются на каждом шаге, без куки и JS
(JS-рендер только opt-in через Kitesurf).

## Поисковые бэкенды и почему так

С lmserver (один IP, без API-ключей) большинство движков закрыты антиботом:
DuckDuckGo по обычному GET отдаёт `202`, Yandex — капчу, Mojeek — капчу,
Brave — `429`, Ecosia — `403`, Marginalia недоступна, Google — согласие/пустую
страницу.

Что работает:

1. **DuckDuckGo HTML через POST + cookie-session** (основной): сервис сначала
   делает GET `https://duckduckgo.com/`, получает cookies в общий cookie-store,
   затем POST-форму на `html.duckduckgo.com/html/` — это возвращает `200` и
   релевантную выдачу. Ссылки приходят в редиректах `duckduckgo.com/l/?uddg=…`,
   сервис их разворачивает.
2. **Bing HTML** (резерв): 10 результатов, но ссылки завёрнуты в
   `bing.com/ck/a?u=a1<base64url>` — декодируются в реальные URL (иначе
   дедупликатор выбрасывал бы всю выдачу).
3. **Bing RSS**: структурированный XML, но выдача бывает скудной.
4. **Wikipedia API**: энциклопедический фолбэк для фактических запросов, без
   ключа и капчи.
5. **SearXNG**, если задан `SEARCH_SEARX_URL`.

Ответ всегда содержит `engines` (что сработало) и `errors` (что и почему нет) —
модель видит, откуда взялись результаты. Сессия DDG «прогревается» не чаще раза
в 6 часов.

Устойчивость к троттлингу:

- выдача каждого движка фильтруется по словам запроса — когда движок
  ограничивает IP, он отдаёт нерелевантную «заглушку» (песни, FACEIT, магазины),
  такие результаты отбрасываются и в ответ попадает
  `"<engine>: N results dropped as irrelevant"`;
- движки сливаются, пока не набрано `count` релевантных результатов, а не
  «первый непустой ответ»;
- после антибот-ответа (`202`, капча, `429`) DuckDuckGo уходит в cooldown
  (`SEARCH_DDG_COOLDOWN`, 300 с по умолчанию) и не дёргается зря; состояние
  видно в `/health` как `ddg_ready`.

## Конфигурация (`search.env`)

| Переменная | По умолчанию | Смысл |
|---|---|---|
| `SEARCH_BIND` | `127.0.0.1:8099` | адрес прослушивания |
| `SEARCH_TOKEN` | пусто | Bearer-токен (пусто = без авторизации) |
| `SEARCH_SEARX_URL` | пусто | свой SearXNG с JSON API |
| `SEARCH_KITESURF_FALLBACK` | `0` | opt-in JS-рендер через kitesurf.dev |
| `SEARCH_CACHE_MB` | 64 | лимит кэша в памяти |
| `SEARCH_RATE_PER_MIN` | 60 | лимит запросов в минуту |
| `SEARCH_MAX_BYTES` | 2 МБ | максимум на страницу |
| `SEARCH_MAX_CHARS` | 20000 | максимум текста в ответе |
| `SEARCH_RESPECT_ROBOTS` | 1 | соблюдать robots.txt |
| `SEARCH_FETCH_TTL` | 21600 | TTL кэша страниц, сек |
| `SEARCH_DDG_COOLDOWN` | 300 | пауза для DuckDuckGo после антибот-ответа, сек |

## Безопасность юнита

`User=searchsvc` (nologin), `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`,
`NoNewPrivileges`, `RestrictAddressFamilies`, `SystemCallFilter=@system-service`,
`MemoryMax=384M`, `CPUQuota=200%`; записывать на диск сервису некуда — кэш в RAM.
