Usuarios distintos a gran escala
Tópicos en Grandes Volúmenes de Datos, Universidad de Concepción
Martín Fuentealba Bizama y Tomás Gutiérrez Bizama

El programa compara HyperLogLog y SetSketch con el conteo exacto de
usuarios distintos por repositorio en GH Archive. Los archivos se
agrupan por día UTC y se excluyen las cuentas con sufijo [bot].

Compilación

En Linux se requiere g++ con C++17 y OpenMP, make y Python 3.11 o superior.
Ejecutar desde la raíz del repositorio.

    make

Ejemplo con los datos de prueba

    python3 src/extraer_eventos.py salida/prueba_bin datos_prueba/muestra-*.json.gz
    OMP_NUM_THREADS=2 ./build/avance salida/prueba salida/prueba_bin/muestra-*.bin
    cat salida/prueba/conteo_exacto.csv

La muestra cubre dos horas, eventos repetidos y una cuenta [bot]. El conteo
exacto debe dar 12 usuarios para los repositorios 101 y 202, y 1 para el 303.
Los repositorios 101 y 202 comparten 6 usuarios. La unión debe dar cero
repositorios con registros distintos para ambos métodos y presupuestos.

Reproducción del avance semanal

El avance usa los archivos del 1 al 7 de marzo de 2024. Ejecutar en Bash
desde la raíz del repositorio. Si los .gz ya están descargados, usar su
ruta en el comando de extracción.

    wget -c -P datos https://data.gharchive.org/2024-03-{01..07}-{0..23}.json.gz
    python3 src/extraer_eventos.py salida/semana_bin datos/2024-03-{01..07}-{0..23}.json.gz
    OMP_NUM_THREADS=8 ./build/avance salida/semana salida/semana_bin/2024-03-{01..07}-{0..23}.bin

Para evaluar otro período, pasar al extractor los .json.gz correspondientes
y al evaluador los .bin resultantes. La evaluación usa bloques en disco y
TMPDIR permite elegir dónde se guardan los temporales.

La carpeta de salida recibe conteo_exacto.csv, datos.csv, cardinalidad.csv,
union.csv e intersecciones.csv. Los presupuestos de 3 y 12 KiB se aplican
al arreglo de registros de cada sketch.

Fuentes

Material del curso, StreamingAleatorizados_Parte2 (HyperLogLog).

Ertl, O. (2021). SetSketch: Filling the Gap between MinHash and HyperLogLog.
PVLDB, 14(11), 2244-2257.
https://doi.org/10.14778/3476249.3476276

Implementación de referencia del paper de SetSketch.
https://github.com/dynatrace-research/set-sketch-paper

Steele, G. L., Lea, D. y Flood, C. H. (2014). Fast splittable pseudorandom
number generators. OOPSLA, 453-472.
https://doi.org/10.1145/2660193.2660195

GH Archive, archivos horarios de eventos públicos de GitHub.
https://www.gharchive.org/
