#include <utility>
#include <stdexcept>
#include <iostream>
#include <vector>
#include <random>

double generateRandomNumber() {
    // Create a random device and a random engine
    static std::random_device rd;   // Random device to seed the engine
    static std::mt19937 gen(rd());  // Mersenne Twister engine seeded with random device

    // Define a uniform distribution between 0 and 1
    static std::uniform_real_distribution<> dis(0.0, 1.0);

    // Generate and return a random number between 0 and 1
    return dis(gen);
}

class MoveBitmask {
    public:
        static std::vector<MoveBitmask> createMasks(int number);
        MoveBitmask() = default;
        MoveBitmask(long index, int size) {
            this->moves_bitmask = new long[size];
            this->size = size;
            for (long* i = this->moves_bitmask; i < (this->moves_bitmask + size); i++) {
                *i = 0;
            }

            this->moves_bitmask[index / (sizeof(long) * 8)] = 1;
            this->moves_bitmask[index / (sizeof(long) * 8)] <<= (index % (sizeof(long) * 8));
        }

        int getSize() {
            return this->size;
        }

        MoveBitmask operator|(MoveBitmask& other) {
            if (other.getSize() != this->getSize()) {
                throw std::invalid_argument("MoveBitmasks must be same size");
            }

            long* out = new long[this->getSize()];

            for (int i = 0; i < this->size; i++) {
                out[i] = this->moves_bitmask[i] | other.moves_bitmask[i];
            }

            return MoveBitmask(out, this->getSize());
        }

        void mutate(float rate) {
            if (generateRandomNumber() <= rate) {
                int index = generateRandomNumber() * size * (sizeof(long) * 8);
                this->moves_bitmask[index / (sizeof(long) * 8)] ^= 1 << (index % (sizeof(long) * 8));
            }
        }

        void print() {
            for (int i = 0; i < this->size; i++) {
                std::cout << this->moves_bitmask[i] << ", ";
            }
            std::cout << "\n";
        }

    private:
        long* moves_bitmask;
        int size;

        MoveBitmask(long* bitmask, int size) {
            this->moves_bitmask = bitmask;
            this->size = size;
        }
};

std::vector<MoveBitmask> MoveBitmask::createMasks(int move_number) {
    int size;

    if (move_number % (sizeof(long) * 8)) {
        size = move_number / (sizeof(long) * 8) + 1;
    } else {
        size = move_number / (sizeof(long) * 8);
    }

    std::vector<MoveBitmask> out;

    for (int i = 0; i < move_number; i++) {
        out.push_back(MoveBitmask(i, size));
    }

    return out;
}