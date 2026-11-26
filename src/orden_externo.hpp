#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <queue>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace tmgvd {

namespace fs = std::filesystem;

class DirectorioTemporal {
public:
    explicit DirectorioTemporal(const fs::path& padre = fs::temp_directory_path()) {
        auto plantilla = (padre / ".tmgvd-XXXXXX").string();
        if (::mkdtemp(plantilla.data()) == nullptr) {
            throw std::system_error(errno, std::generic_category(), "No se pudo crear " + plantilla);
        }
        ruta_ = plantilla;
    }

    DirectorioTemporal(const DirectorioTemporal&) = delete;
    DirectorioTemporal& operator=(const DirectorioTemporal&) = delete;

    ~DirectorioTemporal() {
        std::error_code error;
        fs::remove_all(ruta_, error);
    }

    const fs::path& ruta() const { return ruta_; }

private:
    fs::path ruta_;
};

inline std::ofstream abrir_binario(const fs::path& ruta) {
    std::ofstream salida;
    salida.exceptions(std::ios::badbit | std::ios::failbit);
    salida.open(ruta, std::ios::binary);
    return salida;
}

template <typename T>
void escribir(std::ostream& archivo, const T& registro) {
    static_assert(std::is_trivially_copyable_v<T>);
    archivo.write(reinterpret_cast<const char*>(&registro), sizeof(T));
}

template <typename T>
class Lector {
public:
    explicit Lector(const fs::path& ruta) : archivo_(ruta, std::ios::binary), ruta_(ruta) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (!archivo_) {
            throw std::runtime_error("No se pudo abrir " + ruta_.string());
        }
    }

    bool leer(T& dato) {
        if (archivo_.read(reinterpret_cast<char*>(&dato), sizeof(T))) {
            ++posicion_;
            return true;
        }
        if (!archivo_.eof() || archivo_.bad() || archivo_.gcount() != 0) {
            fallar();
        }
        return false;
    }

    T siguiente() {
        T dato;
        if (!leer(dato)) {
            fallar();
        }
        return dato;
    }

    void leer_bloque(T* datos, std::size_t cantidad) {
        if (!archivo_.read(reinterpret_cast<char*>(datos), cantidad * sizeof(T))) {
            fallar();
        }
        posicion_ += cantidad;
    }

    void ir(std::uint64_t posicion) {
        if (posicion == posicion_ && archivo_) {
            return;
        }
        archivo_.clear();
        archivo_.seekg(static_cast<std::streamoff>(posicion * sizeof(T)));
        if (!archivo_) {
            fallar();
        }
        posicion_ = posicion;
    }

    std::uint64_t posicion() const { return posicion_; }

private:
    [[noreturn]] void fallar() const {
        throw std::runtime_error("Registro incompleto o error de lectura en " + ruta_.string());
    }

    std::ifstream archivo_;
    fs::path ruta_;
    std::uint64_t posicion_ = 0;
};

template <typename T>
class OrdenExterno {
public:
    explicit OrdenExterno(fs::path prefijo, bool distintos = true)
        : prefijo_(std::move(prefijo)), distintos_(distintos) {
        bloque_.reserve(registros_por_bloque);
    }

    void agregar(const T& registro) {
        bloque_.push_back(registro);
        if (bloque_.size() == registros_por_bloque) {
            guardar_bloque();
        }
    }

    fs::path finalizar() {
        guardar_bloque();
        std::vector<T>().swap(bloque_);
        if (archivos_.empty()) {
            archivos_.push_back(nuevo_archivo());
            abrir_binario(archivos_.back()).close();
        }
        while (archivos_.size() > 1) {
            std::vector<fs::path> mezclados;
            for (std::size_t inicio = 0; inicio < archivos_.size(); inicio += vias) {
                const auto fin = std::min(inicio + vias, archivos_.size());
                if (fin == inicio + 1) {
                    mezclados.push_back(archivos_[inicio]);
                    continue;
                }
                auto destino = nuevo_archivo();
                mezclar(inicio, fin, destino);
                for (auto i = inicio; i < fin; ++i) {
                    fs::remove(archivos_[i]);
                }
                mezclados.push_back(std::move(destino));
            }
            archivos_ = std::move(mezclados);
        }
        return archivos_.front();
    }

private:
    // Memoria de ordenamiento por flujo y cantidad máxima de archivos abiertos.
    static constexpr std::size_t registros_por_bloque = 16 * 1024 * 1024 / sizeof(T);
    static constexpr std::size_t vias = 32;

    static bool iguales(const T& a, const T& b) {
        return !(a < b) && !(b < a);
    }

    fs::path nuevo_archivo() {
        return prefijo_.string() + "-" + std::to_string(secuencia_++) + ".bin";
    }

    void guardar_bloque() {
        if (bloque_.empty()) {
            return;
        }
        std::sort(bloque_.begin(), bloque_.end());
        if (distintos_) {
            bloque_.erase(std::unique(bloque_.begin(), bloque_.end(), iguales), bloque_.end());
        }
        archivos_.push_back(nuevo_archivo());
        auto salida = abrir_binario(archivos_.back());
        salida.write(reinterpret_cast<const char*>(bloque_.data()), bloque_.size() * sizeof(T));
        salida.close();
        bloque_.clear();
    }

    struct Nodo {
        T valor;
        std::size_t origen;
        bool operator<(const Nodo& otro) const { return otro.valor < valor; }
    };

    void mezclar(std::size_t inicio, std::size_t fin, const fs::path& destino) const {
        std::vector<Lector<T>> entradas;
        entradas.reserve(fin - inicio);
        std::priority_queue<Nodo> pendientes;
        for (auto i = inicio; i < fin; ++i) {
            entradas.emplace_back(archivos_[i]);
            T valor;
            if (entradas.back().leer(valor)) {
                pendientes.push({valor, entradas.size() - 1});
            }
        }
        auto salida = abrir_binario(destino);
        T anterior{};
        bool hay_anterior = false;
        while (!pendientes.empty()) {
            auto nodo = pendientes.top();
            pendientes.pop();
            if (!distintos_ || !hay_anterior || !iguales(anterior, nodo.valor)) {
                escribir(salida, nodo.valor);
                anterior = nodo.valor;
                hay_anterior = true;
            }
            if (entradas[nodo.origen].leer(nodo.valor)) {
                pendientes.push(nodo);
            }
        }
        salida.close();
    }

    fs::path prefijo_;
    bool distintos_;
    std::size_t secuencia_ = 0;
    std::vector<fs::path> archivos_;
    std::vector<T> bloque_;
};

}  // namespace tmgvd
