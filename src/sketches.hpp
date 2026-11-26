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

class Permutacion {
public:
    explicit Permutacion(std::uint32_t cantidad) : posiciones_(cantidad), marcas_(cantidad) {}

    void iniciar(std::uint64_t hash) {
        estado_ = hash ^ UINT64_C(0xd1b54a32d192ed03);
        if (++generacion_ == 0) {
            std::fill(marcas_.begin(), marcas_.end(), 0);
            generacion_ = 1;
        }
    }

    double uniforme() {
        return (static_cast<double>(siguiente() >> 12) + 0.5) * 0x1p-52;
    }

    // Paso de Fisher-Yates. Las posiciones sin marca aún contienen su índice.
    std::uint32_t extraer(std::uint32_t desde) {
        const auto disponibles = static_cast<std::uint32_t>(posiciones_.size()) - desde;
        const std::uint32_t elegido = desde + indice(disponibles);
        const std::uint32_t salida = marcas_[elegido] == generacion_ ? posiciones_[elegido] : elegido;
        posiciones_[elegido] = marcas_[desde] == generacion_ ? posiciones_[desde] : desde;
        marcas_[elegido] = generacion_;
        return salida;
    }

private:
    std::uint64_t siguiente() {
        estado_ += UINT64_C(0x9e3779b97f4a7c15);
        return mezclar(estado_);
    }

    std::uint32_t indice(std::uint32_t limite) {
        auto producto = (siguiente() >> 32) * limite;
        auto resto = static_cast<std::uint32_t>(producto);
        if (resto < limite) {
            const auto umbral = (0u - limite) % limite;
            while (resto < umbral) {
                producto = (siguiente() >> 32) * limite;
                resto = static_cast<std::uint32_t>(producto);
            }
        }
        return static_cast<std::uint32_t>(producto >> 32);
    }

    std::vector<std::uint32_t> posiciones_;
    std::vector<std::uint32_t> marcas_;
    std::uint32_t generacion_ = 0;
    std::uint64_t estado_ = 0;
};

class ParametrosSetSketch {
public:
    static constexpr double base = 1.001;
    static constexpr double tasa = 20.0;
    static constexpr std::uint16_t maximo = std::numeric_limits<std::uint16_t>::max();

    explicit ParametrosSetSketch(std::uint32_t cantidad)
        : m(cantidad), log_base(std::log(base)), pesos_(std::size_t(maximo) + 1),
          umbrales_(pesos_.size()), intervalos_(m) {
        if (m == 0) {
            throw std::invalid_argument("SetSketch requiere al menos un registro");
        }
        for (std::size_t k = 0; k < pesos_.size(); ++k) {
            pesos_[k] = std::exp(-static_cast<double>(k) * log_base);
        }
        umbrales_[0] = m;
        for (std::size_t k = 1; k < umbrales_.size(); ++k) {
            umbrales_[k] = m * -std::expm1(-tasa * pesos_[k - 1]);
        }
        auto superior = cuantizar(0, 0, maximo);
        for (std::uint32_t j = 0; j < m; ++j) {
            const auto inferior = cuantizar(j + 1.0, 0, superior);
            intervalos_[j] = {inferior, superior};
            superior = inferior;
        }
    }

    std::uint16_t registro(std::uint32_t intervalo, std::uint16_t piso, Permutacion& azar) const {
        const auto [inferior, superior] = intervalos_[intervalo];
        if (superior <= piso || inferior == superior) {
            return superior;
        }
        // La función de distribución transforma cada intervalo
        // equiprobable de la exponencial en un tramo uniforme.
        return cuantizar(intervalo + azar.uniforme(), inferior, superior);
    }

    double peso(std::uint16_t registro) const { return pesos_[registro]; }
    const std::uint32_t m;
    const double log_base;

private:
    std::uint16_t cuantizar(double cuantil, std::uint16_t desde, std::uint16_t hasta) const {
        const auto antes = umbrales_.begin();
        const auto siguiente = std::upper_bound(antes + desde, antes + hasta + 1,
                                                  cuantil, std::greater<double>{});
        return static_cast<std::uint16_t>(siguiente - antes - 1);
    }

    std::vector<double> pesos_;
    std::vector<double> umbrales_;
    std::vector<std::pair<std::uint16_t, std::uint16_t>> intervalos_;
};

class SetSketch {
public:
    SetSketch(const ParametrosSetSketch& parametros, Permutacion& permutacion)
        : parametros_(&parametros), permutacion_(&permutacion), registros_(parametros.m) {}

    std::size_t bytes() const { return registros_.size() * sizeof(registros_[0]); }

    void limpiar() {
        std::fill(registros_.begin(), registros_.end(), 0);
        piso_ = 0;
        cambios_ = 0;
    }

    void insertar(std::uint64_t hash) {
        permutacion_->iniciar(hash);
        for (std::uint32_t j = 0; j < parametros_->m; ++j) {
            const auto nuevo = parametros_->registro(j, piso_, *permutacion_);
            if (nuevo <= piso_) {
                break;
            }
            auto& actual = registros_[permutacion_->extraer(j)];
            if (nuevo > actual) {
                actual = nuevo;
                if (++cambios_ == parametros_->m) {
                    actualizar_piso();
                }
            }
        }
    }

    void unir(const SetSketch& otro) {
        comprobar(otro);
        for (std::size_t i = 0; i < registros_.size(); ++i) {
            registros_[i] = std::max(registros_[i], otro.registros_[i]);
        }
        actualizar_piso();
    }

    bool mismo_estado(const SetSketch& otro) const { return registros_ == otro.registros_; }

    double cardinalidad() const {
        double suma = 0;
        bool lleno = false;
        for (const auto registro : registros_) {
            lleno |= registro != 0;
            suma += parametros_->peso(registro);
        }
        if (!lleno) {
            return 0;
        }
        const double factor = -std::expm1(-parametros_->log_base)
                              / (parametros_->tasa * parametros_->log_base);
        return parametros_->m * factor / suma;
    }

    EstimacionConjunta comparar(const SetSketch& otro) const {
        comprobar(otro);
        const double izquierda = cardinalidad();
        const double derecha = otro.cardinalidad();
        const double total = izquierda + derecha;
        if (izquierda == 0 || derecha == 0) {
            return {total == 0 ? 1.0 : 0.0, 0.0};
        }

        std::size_t mayores = 0;
        std::size_t menores = 0;
        for (std::size_t i = 0; i < registros_.size(); ++i) {
            mayores += registros_[i] > otro.registros_[i];
            menores += registros_[i] < otro.registros_[i];
        }
        const auto iguales = registros_.size() - mayores - menores;
        const double u = izquierda / total;
        const double v = derecha / total;
        const double jmax = std::min(u / v, v / u);
        const auto probabilidad = [&](double x) {
            const double f = (ParametrosSetSketch::base - 1) / ParametrosSetSketch::base;
            return -std::log1p(-std::max(0.0, x) * f) / parametros_->log_base;
        };
        const auto termino = [](std::size_t cantidad, double prob) {
            return cantidad == 0 ? 0.0 : cantidad * std::log(std::max(0.0, prob));
        };
        const auto verosimilitud = [&](double j) {
            const double p_mayor = probabilidad(u - v * j);
            const double p_menor = probabilidad(v - u * j);
            return termino(mayores, p_mayor) + termino(menores, p_menor)
                   + termino(iguales, 1.0 - p_mayor - p_menor);
        };

        double a = 0;
        double b = jmax;
        for (int iteracion = 0; iteracion < 48; ++iteracion) {
            const double j1 = a + (b - a) / 3;
            const double j2 = b - (b - a) / 3;
            if (verosimilitud(j1) < verosimilitud(j2)) {
                a = j1;
            } else {
                b = j2;
            }
        }
        double j = (a + b) / 2;
        for (double extremo : {0.0, jmax}) {
            if (verosimilitud(extremo) >= verosimilitud(j)) {
                j = extremo;
            }
        }
        return {j, total * j / (1.0 + j)};
    }

private:
    void comprobar(const SetSketch& otro) const {
        if (registros_.size() != otro.registros_.size()) {
            throw std::invalid_argument("Los SetSketch deben tener la misma cantidad de registros");
        }
    }

    void actualizar_piso() {
        piso_ = *std::min_element(registros_.begin(), registros_.end());
        cambios_ = 0;
    }

    const ParametrosSetSketch* parametros_;
    Permutacion* permutacion_;
    std::vector<std::uint16_t> registros_;
    std::uint16_t piso_ = 0;
    std::uint32_t cambios_ = 0;
};

}  // namespace tmgvd
