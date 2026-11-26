#include "orden_externo.hpp"
#include "sketches.hpp"

#include <array>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <tuple>

#include <fcntl.h>
#include <omp.h>
#include <sys/file.h>
#include <unistd.h>

namespace {

using Id = std::uint64_t;
using tmgvd::DirectorioTemporal;
using tmgvd::HyperLogLog;
using tmgvd::Lector;
using tmgvd::OrdenExterno;
using tmgvd::ParametrosSetSketch;
using tmgvd::Permutacion;
using tmgvd::SetSketch;
namespace fs = std::filesystem;

constexpr std::array<const char*, 2> metodos{"HLL", "SetSketch"};
constexpr std::array<const char*, 4> rangos{"1", "2-10", "11-100", ">100"};
constexpr std::array<const char*, 3> rangos_j{"0<J<0.01", "0.01<=J<=0.10", "J>0.10"};
constexpr std::array<unsigned, 2> precisiones{12, 14};
constexpr Id horas_por_dia = 24;
constexpr std::array<const char*, 5> resultados{
    "datos.csv", "conteo_exacto.csv", "cardinalidad.csv", "union.csv", "intersecciones.csv"
};

struct Evento {
    Id repo, usuario, hora;

    bool operator<(const Evento& otro) const {
        return std::tie(repo, hora, usuario) < std::tie(otro.repo, otro.hora, otro.usuario);
    }
};

struct UsuarioRepo {
    Id repo, usuario;

    bool operator<(const UsuarioRepo& otro) const {
        return std::tie(repo, usuario) < std::tie(otro.repo, otro.usuario);
    }
};

struct Pertenencia {
    Id usuario;
    std::size_t candidato;

    bool operator<(const Pertenencia& otro) const {
        return std::tie(usuario, candidato) < std::tie(otro.usuario, otro.candidato);
    }
};

struct Repositorio {
    Id id;
    std::uint64_t eventos_inicio, eventos_fin, usuarios_inicio, usuarios_fin;

    std::uint64_t cantidad() const { return usuarios_fin - usuarios_inicio; }
};

struct Datos {
    fs::path eventos, usuarios, repos, invertido;
    std::vector<Repositorio> candidatos;
    std::uint64_t total_eventos = 0;
    std::uint64_t total_repos = 0;
    std::uint64_t usuarios_distintos = 0;
    std::uint64_t pares = 0;
    std::size_t horas = 0;
    Id horas_por_sketch = 1;
    std::array<std::uint64_t, rangos.size()> tamanos{};
};

Id leer_entero(const unsigned char* bytes, std::size_t longitud) {
    Id valor = 0;
    while (longitud != 0) {
        valor = (valor << 8) | bytes[--longitud];
    }
    return valor;
}

std::size_t rango(std::uint64_t n) {
    if (n == 1) return 0;
    if (n <= 10) return 1;
    if (n <= 100) return 2;
    return 3;
}

std::ofstream abrir_csv(const fs::path& ruta) {
    std::ofstream salida;
    salida.exceptions(std::ios::badbit | std::ios::failbit);
    salida.open(ruta);
    salida << std::setprecision(12);
    return salida;
}

void ordenar_entrada(Datos& datos, const fs::path& trabajo, int argc, char* argv[]) {
    OrdenExterno<Evento> eventos(trabajo / "eventos");
    OrdenExterno<UsuarioRepo> usuarios(trabajo / "usuarios");
    std::set<Id> horas;
    for (int i = 2; i < argc; ++i) {
        Lector<std::array<unsigned char, 20>> entrada(argv[i]);
        std::array<unsigned char, 20> registro;
        while (entrada.leer(registro)) {
            const Evento evento{leer_entero(registro.data(), 8),
                                 leer_entero(registro.data() + 8, 8),
                                 leer_entero(registro.data() + 16, 4)};
            eventos.agregar(evento);
            usuarios.agregar({evento.repo, evento.usuario});
            horas.insert(evento.hora);
            ++datos.total_eventos;
        }
    }
    if (datos.total_eventos == 0) {
        throw std::runtime_error("Los archivos no contienen eventos para evaluar");
    }
    datos.horas = horas.size();
    if (*horas.begin() / horas_por_dia != *horas.rbegin() / horas_por_dia) {
        datos.horas_por_sketch = horas_por_dia;
    }
    datos.eventos = eventos.finalizar();
    datos.usuarios = usuarios.finalizar();
}

void indexar_repos(Datos& datos, const fs::path& trabajo, const fs::path& salida) {
    datos.repos = trabajo / "repos.bin";
    auto repos = tmgvd::abrir_binario(datos.repos);
    auto exacto = abrir_csv(salida / "conteo_exacto.csv");
    exacto << "repo_id,usuarios_distintos\n";
    OrdenExterno<Id> distintos(trabajo / "usuarios_distintos");
    OrdenExterno<Pertenencia> invertido(trabajo / "invertido");
    Lector<Evento> eventos(datos.eventos);
    Lector<UsuarioRepo> usuarios(datos.usuarios);
    Lector<UsuarioRepo> por_candidato(datos.usuarios);

    Evento evento;
    UsuarioRepo usuario;
    bool hay_evento = eventos.leer(evento);
    bool hay_usuario = usuarios.leer(usuario);
    while (hay_evento) {
        const auto id = evento.repo;
        if (!hay_usuario || usuario.repo != id) {
            throw std::runtime_error("Los eventos y los usuarios ordenados no coinciden");
        }
        Repositorio repo{id, eventos.posicion() - 1, 0, usuarios.posicion() - 1, 0};
        do {
            hay_evento = eventos.leer(evento);
        } while (hay_evento && evento.repo == id);
        do {
            distintos.agregar(usuario.usuario);
            hay_usuario = usuarios.leer(usuario);
        } while (hay_usuario && usuario.repo == id);
        repo.eventos_fin = eventos.posicion() - hay_evento;
        repo.usuarios_fin = usuarios.posicion() - hay_usuario;

        if (repo.cantidad() >= 11) {
            por_candidato.ir(repo.usuarios_inicio);
            for (auto i = repo.usuarios_inicio; i < repo.usuarios_fin; ++i) {
                invertido.agregar({por_candidato.siguiente().usuario, datos.candidatos.size()});
            }
            datos.candidatos.push_back(repo);
        }
        datos.pares += repo.cantidad();
        ++datos.total_repos;
        ++datos.tamanos[rango(repo.cantidad())];
        tmgvd::escribir(repos, repo);
        exacto << repo.id << ',' << repo.cantidad() << '\n';
    }
    if (hay_usuario) {
        throw std::runtime_error("Hay usuarios ordenados sin un repositorio de eventos");
    }
    repos.close();
    exacto.close();
    datos.usuarios_distintos = fs::file_size(distintos.finalizar()) / sizeof(Id);
    datos.invertido = invertido.finalizar();
}

struct Mediciones {
    std::array<double, 2> directo;
    std::array<double, 2> unido;
    std::array<bool, 2> igual;
};

struct ParDeSketches {
    ParDeSketches(unsigned p, const ParametrosSetSketch& parametros, Permutacion& permutacion)
        : hll(p), setsketch(parametros, permutacion) {}

    void limpiar() {
        hll.limpiar();
        setsketch.limpiar();
    }

    void insertar(Id usuario) {
        const auto hash = tmgvd::hash_usuario(usuario);
        hll.insertar(hash);
        setsketch.insertar(hash);
    }

    void unir(const ParDeSketches& otro) {
        hll.unir(otro.hll);
        setsketch.unir(otro.setsketch);
    }

    HyperLogLog hll;
    SetSketch setsketch;
};

class Evaluador {
public:
    Evaluador(const Datos& datos, unsigned p, const ParametrosSetSketch& parametros)
        : eventos_(datos.eventos), usuarios_(datos.usuarios), permutacion_(parametros.m),
          directo_(p, parametros, permutacion_), unido_(p, parametros, permutacion_),
          parcial_(p, parametros, permutacion_), horas_por_sketch_(datos.horas_por_sketch) {}

    Mediciones medir(const Repositorio& repo) {
        directo_.limpiar();
        unido_.limpiar();
        parcial_.limpiar();
        usuarios_.ir(repo.usuarios_inicio);
        for (auto i = repo.usuarios_inicio; i < repo.usuarios_fin; ++i) {
            directo_.insertar(usuarios_.siguiente().usuario);
        }
        eventos_.ir(repo.eventos_inicio);
        Id periodo = 0;
        for (auto i = repo.eventos_inicio; i < repo.eventos_fin; ++i) {
            const auto evento = eventos_.siguiente();
            const Id periodo_evento = evento.hora / horas_por_sketch_;
            if (i != repo.eventos_inicio && periodo_evento != periodo) {
                unido_.unir(parcial_);
                parcial_.limpiar();
            }
            periodo = periodo_evento;
            parcial_.insertar(evento.usuario);
        }
        unido_.unir(parcial_);

        const std::array<bool, 2> igual{
            directo_.hll.mismo_estado(unido_.hll), directo_.setsketch.mismo_estado(unido_.setsketch)
        };
        const std::array<double, 2> cardinalidad{
            directo_.hll.cardinalidad(), directo_.setsketch.cardinalidad()
        };
        return {cardinalidad,
                {igual[0] ? cardinalidad[0] : unido_.hll.cardinalidad(),
                 igual[1] ? cardinalidad[1] : unido_.setsketch.cardinalidad()}, igual};
    }

private:
    Lector<Evento> eventos_;
    Lector<UsuarioRepo> usuarios_;
    Permutacion permutacion_;
    ParDeSketches directo_, unido_, parcial_;
    const Id horas_por_sketch_;
};

struct Error {
    std::uint64_t n = 0;
    double suma = 0, firmado = 0, mediana = 0, p95 = 0;

    void agregar(double valor) {
        ++n;
        suma += std::abs(valor);
        firmado += valor;
    }

    void percentiles(std::uint64_t posicion, double absoluto) {
        if (posicion == (n - 1) / 2) mediana = absoluto;
        if (posicion == n / 2) mediana = (mediana + absoluto) / 2;
        if (posicion == n - n / 20 - 1) p95 = absoluto;
    }

    void escribir(std::ostream& salida) const {
        salida << n;
        if (n == 0) {
            salida << ",,,,\n";
            return;
        }
        salida << ',' << suma / n << ',' << mediana << ',' << p95 << ',' << firmado / n << '\n';
    }
};

struct Resumen {
    std::array<std::array<Error, rangos.size()>, metodos.size()> por_rango;
    std::array<std::uint64_t, 2> diferencias{};
    std::array<double, 2> error_directo{};
    std::array<double, 2> error_unido{};
};

struct ErrorOrdenado {
    std::size_t grupo;
    double valor;

    bool operator<(const ErrorOrdenado& otro) const {
        return std::tie(grupo, valor) < std::tie(otro.grupo, otro.valor);
    }
};

Resumen medir_cardinalidad(const Datos& datos, unsigned p, const ParametrosSetSketch& parametros,
                          const fs::path& trabajo) {
    constexpr std::size_t tamano_lote = 8192;
    std::vector<std::unique_ptr<Evaluador>> evaluadores;
    for (int i = 0; i < omp_get_max_threads(); ++i) {
        evaluadores.push_back(std::make_unique<Evaluador>(datos, p, parametros));
    }

    Lector<Repositorio> repos(datos.repos);
    std::vector<Repositorio> lote(tamano_lote);
    std::vector<Mediciones> mediciones(tamano_lote);
    Resumen resumen;
    OrdenExterno<ErrorOrdenado> errores(trabajo / ("errores-" + std::to_string(p)), false);
    for (std::uint64_t quedan = datos.total_repos; quedan != 0;) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(quedan, tamano_lote));
        repos.leer_bloque(lote.data(), n);
        quedan -= n;
        std::exception_ptr fallo;

        #pragma omp parallel for schedule(dynamic, 64)
        for (std::size_t i = 0; i < n; ++i) {
            try {
                mediciones[i] = evaluadores[omp_get_thread_num()]->medir(lote[i]);
            } catch (...) {
                #pragma omp critical
                {
                    if (!fallo) {
                        fallo = std::current_exception();
                    }
                }
            }
        }
        if (fallo) {
            std::rethrow_exception(fallo);
        }

        for (std::size_t i = 0; i < n; ++i) {
            const auto exacto = lote[i].cantidad();
            const auto r = rango(exacto);
            for (std::size_t m = 0; m < metodos.size(); ++m) {
                const auto& medida = mediciones[i];
                const double directo = 100.0 * (medida.directo[m] / exacto - 1.0);
                const double unido = 100.0 * (medida.unido[m] / exacto - 1.0);
                resumen.por_rango[m][r].agregar(directo);
                errores.agregar({m * rangos.size() + r, std::abs(directo)});
                resumen.diferencias[m] += !medida.igual[m];
                resumen.error_directo[m] += std::abs(directo);
                resumen.error_unido[m] += std::abs(unido);
            }
        }
    }

    Lector<ErrorOrdenado> ordenados(errores.finalizar());
    std::array<std::uint64_t, metodos.size() * rangos.size()> posiciones{};
    ErrorOrdenado error;
    while (ordenados.leer(error)) {
        auto& estadistica = resumen.por_rango[error.grupo / rangos.size()][error.grupo % rangos.size()];
        estadistica.percentiles(posiciones[error.grupo]++, error.valor);
    }
    return resumen;
}

class IndiceUsuarios {
public:
    explicit IndiceUsuarios(const fs::path& archivo)
        : lector_(archivo), total_(fs::file_size(archivo) / sizeof(Pertenencia)) {
        for (std::uint64_t i = 0; i * filas_por_pagina < total_; ++i) {
            cargar(i);
            ultimos_.push_back(pagina_[longitud_ - 1].usuario);
        }
    }

    template <typename Visitar>
    void posteriores(Id usuario, std::size_t candidato, Visitar visitar) {
        auto it = std::lower_bound(ultimos_.cbegin(), ultimos_.cend(), usuario);
        for (; it != ultimos_.cend(); ++it) {
            cargar(it - ultimos_.cbegin());
            const auto fin = pagina_.cbegin() + longitud_;
            auto fila = std::lower_bound(pagina_.cbegin(), fin, Pertenencia{usuario, candidato + 1});
            for (; fila != fin && fila->usuario == usuario; ++fila) {
                visitar(fila->candidato);
            }
            if (*it != usuario) {
                break;
            }
        }
    }

private:
    static constexpr std::size_t filas_por_pagina = 1024;

    void cargar(std::uint64_t pagina) {
        const auto inicio = pagina * filas_por_pagina;
        if (inicio == inicio_) {
            return;
        }
        longitud_ = std::min<std::uint64_t>(filas_por_pagina, total_ - inicio);
        lector_.ir(inicio);
        lector_.leer_bloque(pagina_.data(), longitud_);
        inicio_ = inicio;
    }

    Lector<Pertenencia> lector_;
    std::uint64_t total_;
    std::vector<Id> ultimos_;
    std::array<Pertenencia, filas_por_pagina> pagina_;
    std::uint64_t inicio_ = std::numeric_limits<std::uint64_t>::max();
    std::size_t longitud_ = 0;
};

struct ParExacto {
    std::size_t a, b;
    std::uint64_t interseccion;
    double jaccard;
};

struct Muestra {
    std::uint64_t elegibles = 0;
    std::vector<ParExacto> pares;
};

using Muestras = std::array<Muestra, rangos_j.size()>;

Muestras seleccionar_pares(const Datos& datos) {
    const std::size_t maximo_por_rango = 120;
    const auto& candidatos = datos.candidatos;
    Lector<UsuarioRepo> usuarios(datos.usuarios);
    IndiceUsuarios indice(datos.invertido);
    Muestras muestras;
    std::mt19937_64 azar(tmgvd::semilla);
    std::vector<std::uint64_t> comunes(candidatos.size());
    std::vector<std::size_t> tocados;
    for (std::size_t a = 0; a < candidatos.size(); ++a) {
        const auto& primero = candidatos[a];
        usuarios.ir(primero.usuarios_inicio);
        for (auto i = primero.usuarios_inicio; i < primero.usuarios_fin; ++i) {
            indice.posteriores(usuarios.siguiente().usuario, a, [&](std::size_t b) {
                if (comunes[b]++ == 0) {
                    tocados.push_back(b);
                }
            });
        }

        // Conserva el orden por ID para que la muestra con semilla fija sea reproducible.
        std::sort(tocados.begin(), tocados.end());
        for (auto b : tocados) {
            const auto interseccion = comunes[b];
            comunes[b] = 0;
            const auto total = primero.cantidad() + candidatos[b].cantidad() - interseccion;
            const auto banda = 100 * interseccion < total ? 0 : 10 * interseccion <= total ? 1 : 2;
            auto& muestra = muestras[banda];
            ++muestra.elegibles;
            const ParExacto par{a, b, interseccion, double(interseccion) / total};
            if (muestra.pares.size() < maximo_por_rango) {
                muestra.pares.push_back(par);
            } else {
                const auto posicion = std::uniform_int_distribution<std::uint64_t>(0, muestra.elegibles - 1)(azar);
                if (posicion < maximo_por_rango) {
                    muestra.pares[posicion] = par;
                }
            }
        }
        tocados.clear();
    }
    return muestras;
}

struct ErrorConjunto {
    double jaccard = 0;
    double interseccion = 0;
};

using ResumenConjunto = std::array<std::array<ErrorConjunto, metodos.size()>, rangos_j.size()>;

ResumenConjunto medir_pares(const Datos& datos, const Muestras& muestras,
                           unsigned p, const ParametrosSetSketch& parametros) {
    Lector<UsuarioRepo> usuarios(datos.usuarios);
    Permutacion permutacion(parametros.m);
    std::map<std::size_t, ParDeSketches> sketches;
    const auto obtener = [&](std::size_t indice) -> const ParDeSketches& {
        const auto [iterador, nuevo] = sketches.try_emplace(indice, p, parametros, permutacion);
        auto& sketch = iterador->second;
        if (nuevo) {
            const auto& repo = datos.candidatos[indice];
            usuarios.ir(repo.usuarios_inicio);
            for (auto i = repo.usuarios_inicio; i < repo.usuarios_fin; ++i) {
                sketch.insertar(usuarios.siguiente().usuario);
            }
        }
        return sketch;
    };

    ResumenConjunto resumen{};
    for (std::size_t r = 0; r < muestras.size(); ++r) {
        for (const auto& par : muestras[r].pares) {
            const auto& a = obtener(par.a);
            const auto& b = obtener(par.b);
            const std::array estimaciones{a.hll.comparar(b.hll), a.setsketch.comparar(b.setsketch)};
            for (std::size_t m = 0; m < metodos.size(); ++m) {
                auto& error = resumen[r][m];
                error.jaccard += std::abs(estimaciones[m].jaccard - par.jaccard);
                error.interseccion += std::abs(estimaciones[m].interseccion - par.interseccion);
            }
        }
    }
    return resumen;
}

void guardar_datos(const fs::path& carpeta, const Datos& datos, int archivos) {
    auto resumen = abrir_csv(carpeta / "datos.csv");
    resumen << "metrica,valor\n"
            << "archivos," << archivos << "\nhoras_utc," << datos.horas
            << "\neventos_incluidos," << datos.total_eventos
            << "\nrepositorios," << datos.total_repos
            << "\nusuarios_distintos," << datos.usuarios_distintos
            << "\npares_repo_usuario," << datos.pares << '\n';
    for (std::size_t i = 0; i < rangos.size(); ++i) {
        resumen << "repos_" << rangos[i] << ',' << datos.tamanos[i] << '\n';
    }
    resumen.close();
}

void evaluar(const Datos& datos, const Muestras& muestras, const fs::path& trabajo,
             const fs::path& salida) {
    std::cerr << "Unión por " << (datos.horas_por_sketch == 1 ? "horas" : "días") << " UTC\n";
    auto e1 = abrir_csv(salida / "cardinalidad.csv");
    auto e2 = abrir_csv(salida / "union.csv");
    auto e3 = abrir_csv(salida / "intersecciones.csv");
    e1 << "bytes,metodo,rango_usuarios,repositorios,error_medio_pct,mediana_pct,p95_pct,sesgo_pct\n";
    e2 << "bytes,metodo,repositorios,estados_distintos,error_directo_medio_pct,error_unido_medio_pct\n";
    e3 << "bytes,metodo,rango_j,pares_elegibles,pares_medidos,mae_j,mae_interseccion\n";
    for (auto p : precisiones) {
        const auto bytes = HyperLogLog(p).bytes();
        const ParametrosSetSketch parametros(static_cast<std::uint32_t>(bytes / sizeof(std::uint16_t)));
        std::cerr << "Evaluando " << bytes << " bytes por sketch\n";
        const auto resumen = medir_cardinalidad(datos, p, parametros, trabajo);
        const auto conjuntas = medir_pares(datos, muestras, p, parametros);
        for (std::size_t m = 0; m < metodos.size(); ++m) {
            for (std::size_t r = 0; r < rangos.size(); ++r) {
                e1 << bytes << ',' << metodos[m] << ',' << rangos[r] << ',';
                resumen.por_rango[m][r].escribir(e1);
            }
            const auto n = datos.total_repos;
            e2 << bytes << ',' << metodos[m] << ',' << n << ',' << resumen.diferencias[m]
               << ',' << resumen.error_directo[m] / n << ',' << resumen.error_unido[m] / n << '\n';
            for (std::size_t r = 0; r < rangos_j.size(); ++r) {
                const auto cantidad = muestras[r].pares.size();
                e3 << bytes << ',' << metodos[m] << ',' << rangos_j[r] << ','
                   << muestras[r].elegibles << ',' << cantidad << ',';
                if (cantidad != 0) {
                    const auto& error = conjuntas[r][m];
                    e3 << error.jaccard / cantidad << ',' << error.interseccion / cantidad << '\n';
                } else {
                    e3 << ",\n";
                }
            }
        }
        std::cerr << "Completado " << bytes << " bytes por sketch\n";
    }
    e1.close();
    e2.close();
    e3.close();
}

void publicar(const fs::path& temporal, const fs::path& salida) {
    const int fd = ::open(salida.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd == -1) {
        throw std::system_error(errno, std::generic_category(), "No se pudo abrir " + salida.string());
    }
    try {
        if (::flock(fd, LOCK_EX) == -1) {
            throw std::system_error(errno, std::generic_category(), "No se pudo bloquear " + salida.string());
        }
        for (const auto* nombre : resultados) {
            if (fs::is_directory(salida / nombre)) {
                throw std::runtime_error("El destino es una carpeta: " + (salida / nombre).string());
            }
        }
        for (const auto* nombre : resultados) {
            fs::rename(temporal / nombre, salida / nombre);
        }
    } catch (...) {
        ::close(fd);
        throw;
    }
    ::close(fd);
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Uso: avance carpeta_salida hora0.bin [hora1.bin ...]\n";
        return 1;
    }
    try {
        const fs::path salida(argv[1]);
        fs::create_directories(salida);
        const DirectorioTemporal trabajo;
        const DirectorioTemporal temporal(salida);
        Datos datos;
        ordenar_entrada(datos, trabajo.ruta(), argc, argv);
        indexar_repos(datos, trabajo.ruta(), temporal.ruta());
        std::cerr << datos.total_eventos << " eventos en " << datos.total_repos << " repositorios\n";
        guardar_datos(temporal.ruta(), datos, argc - 2);
        evaluar(datos, seleccionar_pares(datos), trabajo.ruta(), temporal.ruta());
        publicar(temporal.ruta(), salida);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
