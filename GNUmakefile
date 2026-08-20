LOCAL_COMPOSE = docker compose -f docker-compose.yml -f docker-compose.local.yml

.PHONY: local-dev local-dev-logs local-dev-shell local-dev-down

local-dev:
	$(LOCAL_COMPOSE) up -d --build --force-recreate backend
	$(LOCAL_COMPOSE) exec -T backend /bin/sh /app/docker/dev/wait-ready.sh

local-dev-logs:
	$(LOCAL_COMPOSE) logs -f backend

local-dev-shell:
	$(LOCAL_COMPOSE) exec backend bash

local-dev-down:
	$(LOCAL_COMPOSE) down
