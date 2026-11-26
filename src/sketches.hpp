#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tmgvd {

inline constexpr std::uint64_t semilla = 20240304;

// Mezcla de 64 bits de SplitMix64 (Steele, Lea y Flood, 2014).
inline std::uint64_t mezclar(std::uint64_t valor) {
    valor = (valor ^ (valor >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    valor = (valor ^ (valor >> 27)) * UINT64_C(0x94d049bb133111eb);
    return valor ^ (valor >> 31);
}

inline std::uint64_t hash_usuario(std::uint64_t usuario) {
    return mezclar(usuario + semilla);
}

struct EstimacionConjunta {
    double jaccard;
    double interseccion;
};

class HyperLogLog {
public:
    explicit HyperLogLog(unsigned p) : p_(p), m_(cantidad(p)), registros_(3 * m_ / 4) {}

    std::size_t bytes() const { return registros_.size(); }
    void limpiar() { std::fill(registros_.begin(), registros_.end(), 0); }

    void insertar(std::uint64_t hash) {
        const auto indice = static_cast<std::size_t>(hash >> (64 - p_));
        const auto sufijo = hash << p_;
        const unsigned rango = sufijo == 0 ? 65 - p_ : 1 + __builtin_clzll(sufijo);
        const auto grupo = 3 * (indice / 4);
        const unsigned desplazamiento = 6 * (indice % 4);
        const unsigned palabra = leer(grupo);
        if (rango > ((palabra >> desplazamiento) & 63u)) {
            escribir(grupo, (palabra & ~(63u << desplazamiento)) | (rango << desplazamiento));
        }
    }

    void unir(const HyperLogLog& otro) {
        comprobar(otro);
        for (std::size_t grupo = 0; grupo < bytes(); grupo += 3) {
            const unsigned derecha = otro.leer(grupo);
            if (derecha == 0) {
                continue;
            }
            const unsigned izquierda = leer(grupo);
            unsigned maxima = 0;
            for (unsigned posicion = 0; posicion < 24; posicion += 6) {
                maxima |= std::max((izquierda >> posicion) & 63u,
                                   (derecha >> posicion) & 63u) << posicion;
            }
            escribir(grupo, maxima);
        }
    }

    bool mismo_estado(const HyperLogLog& otro) const { return registros_ == otro.registros_; }

    double cardinalidad() const {
        static const auto pesos = [] {
            std::array<double, 64> tabla{};
            for (int i = 0; i < 64; ++i) {
                tabla[i] = std::ldexp(1.0, -i);
            }
            return tabla;
        }();
        std::size_t ceros = m_;
        double suma = 0;
        for (std::size_t grupo = 0; grupo < bytes(); grupo += 3) {
            unsigned palabra = leer(grupo);
            while (palabra != 0) {
                const unsigned registro = palabra & 63u;
                if (registro != 0) {
                    --ceros;
                    suma += pesos[registro];
                }
                palabra >>= 6;
            }
        }
        if (ceros == m_) {
            return 0;
        }
        const double m = static_cast<double>(m_);
        const double alfa = m_ == 16 ? 0.673 : m_ == 32 ? 0.697
                            : m_ == 64 ? 0.709 : 0.7213 / (1.0 + 1.079 / m);
        const double estimacion = alfa * m * m / (suma + ceros);
        if (estimacion <= 2.5 * m && ceros != 0) {
            return -m * std::log1p(-static_cast<double>(m_ - ceros) / m);
        }
        return estimacion;
    }

    EstimacionConjunta comparar(const HyperLogLog& otro) const {
        auto conjunto = *this;
        conjunto.unir(otro);
        const double izquierda = cardinalidad();
        const double derecha = otro.cardinalidad();
        const double inter = std::clamp(izquierda + derecha - conjunto.cardinalidad(),
                                        0.0, std::min(izquierda, derecha));
        const double total = izquierda + derecha - inter;
        return {total == 0 ? 1.0 : inter / total, inter};
    }

private:
    static std::size_t cantidad(unsigned p) {
        if (p < 4 || p > 20) {
            throw std::invalid_argument("La precisión de HLL debe estar entre 4 y 20");
        }
        return std::size_t{1} << p;
    }

    void comprobar(const HyperLogLog& otro) const {
        if (p_ != otro.p_) {
            throw std::invalid_argument("Los sketches HLL deben tener la misma precisión");
        }
    }

    unsigned leer(std::size_t i) const {
        return unsigned(registros_[i]) | (unsigned(registros_[i + 1]) << 8)
               | (unsigned(registros_[i + 2]) << 16);
    }

    void escribir(std::size_t i, unsigned palabra) {
        registros_[i] = palabra & 255u;
        registros_[i + 1] = (palabra >> 8) & 255u;
        registros_[i + 2] = palabra >> 16;
    }

    unsigned p_;
    std::size_t m_;
    std::vector<std::uint8_t> registros_;
};

}  // namespace tmgvd
