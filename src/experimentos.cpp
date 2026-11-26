#include "orden_externo.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <tuple>

namespace {

using Id = std::uint64_t;
namespace fs = std::filesystem;

struct UsuarioRepo {
    Id repo, usuario;

    bool operator<(const UsuarioRepo& otro) const {
        return std::tie(repo, usuario) < std::tie(otro.repo, otro.usuario);
    }
};

Id leer_entero(const unsigned char* bytes, std::size_t longitud) {
    Id valor = 0;
    while (longitud != 0) {
        valor = (valor << 8) | bytes[--longitud];
    }
    return valor;
}

fs::path ordenar_usuarios(const fs::path& trabajo, int argc, char* argv[]) {
    tmgvd::OrdenExterno<UsuarioRepo> usuarios(trabajo / "usuarios");
    for (int i = 2; i < argc; ++i) {
        tmgvd::Lector<std::array<unsigned char, 20>> entrada(argv[i]);
        std::array<unsigned char, 20> registro;
        while (entrada.leer(registro)) {
            usuarios.agregar({leer_entero(registro.data(), 8),
                              leer_entero(registro.data() + 8, 8)});
        }
    }
    return usuarios.finalizar();
}

void guardar_conteo(const fs::path& ordenados, const fs::path& ruta) {
    tmgvd::Lector<UsuarioRepo> usuarios(ordenados);
    UsuarioRepo actual;
    bool hay_usuario = usuarios.leer(actual);
    if (!hay_usuario) {
        throw std::runtime_error("Los archivos no contienen eventos para evaluar");
    }

    std::ofstream exacto;
    exacto.exceptions(std::ios::badbit | std::ios::failbit);
    exacto.open(ruta);
    exacto << "repo_id,usuarios_distintos\n";
    while (hay_usuario) {
        const auto repo = actual.repo;
        std::uint64_t cantidad = 0;
        do {
            ++cantidad;
            hay_usuario = usuarios.leer(actual);
        } while (hay_usuario && actual.repo == repo);
        exacto << repo << ',' << cantidad << '\n';
    }
    exacto.close();
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
        const tmgvd::DirectorioTemporal trabajo;
        const tmgvd::DirectorioTemporal temporal(salida);
        const auto nombre = "conteo_exacto.csv";
        const auto ordenados = ordenar_usuarios(trabajo.ruta(), argc, argv);
        guardar_conteo(ordenados, temporal.ruta() / nombre);
        fs::rename(temporal.ruta() / nombre, salida / nombre);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
