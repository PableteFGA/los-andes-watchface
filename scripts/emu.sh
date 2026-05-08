#!/bin/bash
# emu.sh — Lanza el emulador Pebble limpiando estado huérfano
# Uso: ./scripts/emu.sh [install|config|logs] [gabbro|emery|basalt|aplite]
#   install  (default) — instala la app en el emulador
#   config   — instala y abre la página de configuración
#   logs     — instala y muestra los logs en tiempo real
# Plataforma por defecto: gabbro

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"

ACTION="${1:-install}"
PLATFORM="${2:-gabbro}"

case "$PLATFORM" in
    gabbro|emery|basalt|aplite) ;;
    *)
        echo "[emu] Plataforma desconocida: '$PLATFORM'. Usa: gabbro, emery, basalt, aplite"
        exit 1
        ;;
esac

echo "[emu] Plataforma: $PLATFORM | Acción: $ACTION"

echo "[emu] Limpiando procesos y estado huérfano..."
pkill -f qemu-pebble 2>/dev/null
pkill -f pypkjs 2>/dev/null
sleep 1
rm -f /tmp/pb-emulator.json

echo "[emu] Limpiando flash del emulador..."
pebble wipe 2>/dev/null

echo "[emu] Compilando..."
cd "$REPO_DIR"
pebble build || { echo "[emu] ERROR: build falló"; exit 1; }

echo "[emu] Instalando en emulador $PLATFORM..."
pebble install --emulator "$PLATFORM" || { echo "[emu] ERROR: install falló"; exit 1; }

case "$ACTION" in
    config)
        echo "[emu] Abriendo configuración (local)..."
        pebble emu-app-config --file "$REPO_DIR/scripts/clay-config.html" --emulator "$PLATFORM"
        ;;
    logs)
        echo "[emu] Mostrando logs (Ctrl+C para salir)..."
        pebble logs --emulator "$PLATFORM"
        ;;
    install)
        echo "[emu] Listo."
        ;;
    *)
        echo "Uso: $0 [install|config|logs] [gabbro|emery|basalt|aplite]"
        exit 1
        ;;
esac
