LOCAL_COMPOSE = docker compose -f docker-compose.yml -f docker-compose.local.yml
LOCAL_GPU_COMPOSE = $(LOCAL_COMPOSE) -f docker-compose.local-gpu.yml

.PHONY: local-dev local-dev-gpu local-dev-logs local-dev-shell local-dev-down

local-dev:
	$(LOCAL_COMPOSE) up -d --build --force-recreate backend
	$(LOCAL_COMPOSE) exec -T backend /bin/sh /app/docker/dev/wait-ready.sh

local-dev-gpu:
	$(LOCAL_GPU_COMPOSE) up -d --build --force-recreate backend
	$(LOCAL_GPU_COMPOSE) exec -T backend /bin/sh /app/docker/dev/wait-ready.sh

local-dev-logs:
	$(LOCAL_COMPOSE) logs -f backend

local-dev-shell:
	$(LOCAL_COMPOSE) exec backend bash

local-dev-down:
	$(LOCAL_COMPOSE) down
