#include <utility>
#include <stdexcept>

class MoveBitmask {
    public:
        static MoveBitmask* createMasks(int number);
        MoveBitmask() = default;
        MoveBitmask(long index, int size) {
            this->moves_bitmask = new long[size];
            this->size = size;
            for (long* i = this->moves_bitmask; i < (this->moves_bitmask + size); i++) {
                *i = 0;
            }
            this->moves_bitmask[index / sizeof(long) * 8] = 1;
            this->moves_bitmask[index / sizeof(long) * 8] <<= (index % sizeof(long) * 8);
        }

        int getSize() {
            return this->size;
        }

        MoveBitmask operator|(MoveBitmask *other) {
            if (other->getSize() != this->getSize()) {
                throw std::invalid_argument("BigBitmasks must be same size");
            }

            long* out = new long[this->getSize()];

            for (int i = 0; i < this->size; i++) {
                out[i] = this->moves_bitmask[i] | other->moves_bitmask[i];
            }

            return MoveBitmask(out, this->getSize());
        }

        void mutate(float rate) {
            if (rand() >= rate) {
                int index = rand() * size * (sizeof(long) * 8);
                this->moves_bitmask[index / sizeof(long) * 8] ^= 1 << (index % sizeof(long) * 8);
            }
        }

    private:
        long* moves_bitmask;
        int size;

        MoveBitmask(long* bitmask, int size) {
            this->moves_bitmask = bitmask;
            this->size = size;
        }
};

MoveBitmask* MoveBitmask::createMasks(int move_number) {
    int size;

    if (move_number % sizeof(long) * 8) {
        size = move_number / (sizeof(long) * 8) + 1;
    } else {
        size = move_number / (sizeof(long) * 8);
    }

    MoveBitmask* out = new MoveBitmask[move_number];

    for (int i = 1; i < move_number + 1; i++) {
        out[i] = MoveBitmask(i, size);
    }

    return out;
}