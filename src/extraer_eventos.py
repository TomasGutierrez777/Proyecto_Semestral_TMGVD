import argparse
import gzip
import json
import struct
from datetime import datetime
from pathlib import Path
from tempfile import NamedTemporaryFile


# Cada registro ocupa 20 bytes en little-endian: repo, usuario y hora UTC
# como número de horas transcurridas desde 1970-01-01 00:00 UTC.
REGISTRO = struct.Struct("<QQI")


def extraer(archivo, destino):
    total = bots = invalidos = 0
    temporal = NamedTemporaryFile(dir=destino.parent, delete=False)
    ruta_temporal = Path(temporal.name)

    try:
        with temporal as salida, gzip.open(archivo, "rb") as entrada:
            for linea in entrada:
                total += 1
                try:
                    evento = json.loads(linea)
                    actor = evento["actor"]
                    login = actor["login"]
                    repo = evento["repo"]["id"]
                    usuario = actor["id"]
                    if not isinstance(login, str) or type(repo) is not int or type(usuario) is not int:
                        invalidos += 1
                        continue
                    if login.endswith("[bot]"):
                        bots += 1
                        continue
                    fecha = datetime.fromisoformat(evento["created_at"])
                    if fecha.tzinfo is None:
                        invalidos += 1
                        continue
                    hora = int(fecha.timestamp() // 3600)
                    salida.write(REGISTRO.pack(repo, usuario, hora))
                except (KeyError, TypeError, ValueError, OverflowError, struct.error):
                    invalidos += 1

        ruta_temporal.replace(destino)
    finally:
        ruta_temporal.unlink(missing_ok=True)

    return total, bots, invalidos


def main():
    parser = argparse.ArgumentParser(description="Extraer repositorios, usuarios y horas de GH Archive.")
    parser.add_argument("salida", type=Path, help="Carpeta para los archivos binarios")
    parser.add_argument("archivos", type=Path, nargs="+", help="Archivos horarios .json.gz")
    args = parser.parse_args()

    nombres = [archivo.name.removesuffix(".json.gz") for archivo in args.archivos]
    if len(nombres) != len(set(nombres)):
        parser.error("Hay archivos de entrada con el mismo nombre.")

    for archivo in args.archivos:
        if not archivo.name.endswith(".json.gz") or not archivo.is_file():
            parser.error(f"No es un archivo horario .json.gz: {archivo}")

    args.salida.mkdir(parents=True, exist_ok=True)
    total = bots = invalidos = 0
    for archivo, nombre in zip(args.archivos, nombres):
        try:
            eventos, descartados, sin_campos = extraer(archivo, args.salida / f"{nombre}.bin")
        except (EOFError, OSError) as error:
            parser.exit(1, f"{archivo}: {error}\n")
        total += eventos
        bots += descartados
        invalidos += sin_campos
        print(f"{archivo.name}: {eventos - descartados - sin_campos} eventos incluidos", flush=True)

    print(f"Archivos: {len(args.archivos)}")
    print(f"Eventos leídos: {total}")
    print(f"Eventos de cuentas [bot]: {bots}")
    print(f"Eventos sin campos válidos: {invalidos}")
    print(f"Eventos incluidos: {total - bots - invalidos}")


if __name__ == "__main__":
    main()
