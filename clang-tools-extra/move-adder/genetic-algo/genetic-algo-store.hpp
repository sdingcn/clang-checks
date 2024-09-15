#include "move-bitmask.hpp"
#include <vector>
#include <utility>
#include <fstream>
#include <iostream>
#include <string>


class TestStore {
    public:
        std::vector<MoveBitmask> moveTestMap;
        std::vector<MoveBitmask> moveBitmask;
        std::vector<std::string> idxTestMap;
        std::unordered_map<std::string, long> moveIdxMap;

        TestStore(long numMoves, int numTestFiles, std::string movesTxtPath, std::unordered_map<std::string, long> moveIdxMap) {
            this->moveBitmask = MoveBitmask::createMasks(numMoves);
            this->moveTestMap = MoveBitmask::createMasksEmpty(numTestFiles);
            this->moveIdxMap = moveIdxMap;

            std::ifstream MovesTxt(movesTxtPath);

            if (MovesTxt.is_open()) {
                std::string line;
                while (std::getline(MovesTxt, line)) {
                    bool isTest = false;
                    if (line.at(0) == '{') {
                        idxTestMap.push_back(line.substr(1, line.length() - 1));
                    } else {
                        std::string move = line.substr(1, line.length() - 1);
                        auto x = moveTestMap[(*moveIdxMap.find(move)).second];
                        MoveBitmask mb = MoveBitmask(idxTestMap.size(), numTestFiles);
                        moveTestMap[(*moveIdxMap.find(move)).second] = moveTestMap[(*moveIdxMap.find(move)).second] | mb;
                    }
                
                }
            }
        }

        MoveBitmask combineMoves(std::string s1, std::string s2) {

        }
        
};