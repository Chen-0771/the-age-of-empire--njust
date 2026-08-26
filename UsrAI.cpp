#include "UsrAI.h"
#include<set>
#include <iostream>
#include<unordered_map>
#include<list>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

using namespace std;
tagGame tagUsrGame;
ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

namespace
{
    // ===== 持久状态 =====
    // processData 每帧都会被调用，所以这些状态必须跨帧保存。
    int gLastThinkFrame = -1000;
    int gLastBuildTryFrame = -1000;
    int gBuildSearchSeed = 0;
    int gBronzeFrame = -1;
    int gExploreStep = 0;

    set<int> gTechRequested;
    map<int, int> gLastOrderFrame;      // SN -> 最近一次发命令的帧

    // ---------- 基础工具 ----------
    int countBuilding(const tagInfo &info, int type)
    {
        int n = 0;
        for (const tagBuilding &b : info.buildings)
            if (b.Type == type) ++n;
        return n;
    }

    tagBuilding* findBuilding(tagInfo &info, int type)
    {
        for (tagBuilding &b : info.buildings)
            if (b.Type == type) return &b;
        return nullptr;
    }

    const tagBuilding* findBuildingConst(const tagInfo &info, int type)
    {
        for (const tagBuilding &b : info.buildings)
            if (b.Type == type) return &b;
        return nullptr;
    }

    int populationUsed(const tagInfo &info)
    {
        // 内核值能正确处理后勤科技带来的 0.5 人口。
        return static_cast<int>(std::ceil(info.Human_Num));
    }

    int militaryCount(const tagInfo &info)
    {
        int n = 0;
        for (const tagArmy &a : info.armies)
            if (a.Sort != AT_PRIEST) ++n;
        return n;
    }

    double blockDistance(int dr1, int ur1, int dr2, int ur2)
    {
        double x = static_cast<double>(dr1 - dr2);
        double y = static_cast<double>(ur1 - ur2);
        return std::sqrt(x * x + y * y);
    }

    bool canOrder(int sn, int frame, int gap = 20)
    {
        map<int,int>::iterator it = gLastOrderFrame.find(sn);
        return it == gLastOrderFrame.end() || frame - it->second >= gap;
    }

    void markOrdered(int sn, int frame)
    {
        gLastOrderFrame[sn] = frame;
    }

    int buildingSize(int type)
    {
        if (type == BUILDING_HOME || type == BUILDING_ARROWTOWER) return 2;
        return 3;
    }

    bool overlaps(int x1, int y1, int s1, int x2, int y2, int s2)
    {
        return !(x1 + s1 <= x2 || x2 + s2 <= x1 ||
                 y1 + s1 <= y2 || y2 + s2 <= y1);
    }

    bool legalBuildPlace(const tagInfo &info, int type, int x, int y)
    {
        const int s = buildingSize(type);
        if (!info.theMap) return false;
        if (x < 1 || y < 1 || x + s >= MAP_L || y + s >= MAP_U) return false;

        // 必须全部为已探索平坦草地，而且高度一致。
        int h = (*info.theMap)[x][y].height;
        for (int i = x; i < x + s; ++i)
        {
            for (int j = y; j < y + s; ++j)
            {
                const tagTerrain &t = (*info.theMap)[i][j];
                if (t.type != MAPPATTERN_GRASS || t.height != h) return false;
            }
        }

        // 避开现有建筑。
        for (const tagBuilding &b : info.buildings)
        {
            if (overlaps(x, y, s, b.BlockDR, b.BlockUR, buildingSize(b.Type)))
                return false;
        }

        // 避开块资源。资源具体占地不完全一致，这里保守按 1 格处理。
        for (const tagResource &r : info.resources)
        {
            if (r.BlockDR >= x && r.BlockDR < x + s &&
                r.BlockUR >= y && r.BlockUR < y + s)
                return false;
        }

        // 避开当前人物所在格，减少 HumanBuild 因碰撞失败的概率。
        for (const tagFarmer &f : info.farmers)
        {
            if (f.BlockDR >= x && f.BlockDR < x + s &&
                f.BlockUR >= y && f.BlockUR < y + s)
                return false;
        }
        for (const tagArmy &a : info.armies)
        {
            if (a.BlockDR >= x && a.BlockDR < x + s &&
                a.BlockUR >= y && a.BlockUR < y + s)
                return false;
        }
        return true;
    }

    bool findBuildPlace(const tagInfo &info, int type, int &outX, int &outY)
    {
        const tagBuilding *center = findBuildingConst(info, BUILDING_CENTER);
        if (!center) return false;

        const int cx = center->BlockDR;
        const int cy = center->BlockUR;

        // 以市镇中心为圆心逐圈搜索，seed 让失败后的下一次从不同位置开始。
        for (int radius = 4; radius <= 24; ++radius)
        {
            for (int k = 0; k < 8 * radius; ++k)
            {
                int p = (k + gBuildSearchSeed) % (8 * radius);
                int dx = 0, dy = 0;
                if (p < 2 * radius)          { dx = -radius + p; dy = -radius; }
                else if (p < 4 * radius)     { dx = radius; dy = -radius + (p - 2 * radius); }
                else if (p < 6 * radius)     { dx = radius - (p - 4 * radius); dy = radius; }
                else                          { dx = -radius; dy = radius - (p - 6 * radius); }

                int x = cx + dx;
                int y = cy + dy;
                if (legalBuildPlace(info, type, x, y))
                {
                    outX = x;
                    outY = y;
                    gBuildSearchSeed += 7;
                    return true;
                }
            }
        }
        return false;
    }

    tagFarmer* idleBuilder(tagInfo &info)
    {
        tagFarmer *best = nullptr;
        const tagBuilding *center = findBuildingConst(info, BUILDING_CENTER);
        double bestD = numeric_limits<double>::max();

        for (tagFarmer &f : info.farmers)
        {
            if (f.FarmerSort != FARMERTYPE_FARMER || f.NowState != HUMAN_STATE_IDLE) continue;
            double d = center ? blockDistance(f.BlockDR, f.BlockUR, center->BlockDR, center->BlockUR) : 0.0;
            if (!best || d < bestD)
            {
                best = &f;
                bestD = d;
            }
        }
        return best;
    }

    bool tryBuild(UsrAI *ai, tagInfo &info, int type, int requiredWood, int requiredStone = 0)
    {
        if (info.GameFrame - gLastBuildTryFrame < UsrAIConfig::BUILD_RETRY_INTERVAL) return false;
        if (info.Wood < requiredWood || info.Stone < requiredStone) return false;

        tagFarmer *builder = idleBuilder(info);
        if (!builder) return false;

        int x = -1, y = -1;
        if (!findBuildPlace(info, type, x, y)) return false;

        ai->HumanBuild(builder->SN, type, x, y);
        markOrdered(builder->SN, info.GameFrame);
        gLastBuildTryFrame = info.GameFrame;
        return true;
    }

    // ---------- 资源调度 ----------
    int currentWorkersOnType(const tagInfo &info, int resourceType)
    {
        set<int> sns;
        for (const tagResource &r : info.resources)
            if (r.Type == resourceType && r.Cnt > 0) sns.insert(r.SN);

        int n = 0;
        for (const tagFarmer &f : info.farmers)
            if (f.FarmerSort == FARMERTYPE_FARMER && sns.count(f.WorkObjectSN)) ++n;
        return n;
    }

    int currentFarmWorkers(const tagInfo &info)
    {
        set<int> farmSN;
        for (const tagBuilding &b : info.buildings)
            if (b.Type == BUILDING_FARM && b.Cnt > 0) farmSN.insert(b.SN);
        int n = 0;
        for (const tagFarmer &f : info.farmers)
            if (farmSN.count(f.WorkObjectSN)) ++n;
        return n;
    }

    int chooseNearestResource(const tagInfo &info, const tagFarmer &f, int type)
    {
        int bestSN = -1;
        double best = numeric_limits<double>::max();
        for (const tagResource &r : info.resources)
        {
            if (r.Type != type || r.Cnt <= 0) continue;
            double d = blockDistance(f.BlockDR, f.BlockUR, r.BlockDR, r.BlockUR);
            if (d < best)
            {
                best = d;
                bestSN = r.SN;
            }
        }
        return bestSN;
    }

    int chooseNearestFarm(const tagInfo &info, const tagFarmer &f)
    {
        int bestSN = -1;
        double best = numeric_limits<double>::max();
        for (const tagBuilding &b : info.buildings)
        {
            if (b.Type != BUILDING_FARM || b.Cnt <= 0) continue;
            double d = blockDistance(f.BlockDR, f.BlockUR, b.BlockDR, b.BlockUR);
            if (d < best)
            {
                best = d;
                bestSN = b.SN;
            }
        }
        return bestSN;
    }

    int chooseFoodTarget(const tagInfo &info, const tagFarmer &f)
    {
        // 浆果最安全；其次农田；再次瞪羚。
        int sn = chooseNearestResource(info, f, RESOURCE_BUSH);
        if (sn != -1) return sn;
        sn = chooseNearestFarm(info, f);
        if (sn != -1) return sn;
        sn = chooseNearestResource(info, f, RESOURCE_GAZELLE);
        if (sn != -1) return sn;
        // 后期如果附近资源不足，再允许猎象（收益高但有风险）。
        if (info.GameFrame > 9000)
            sn = chooseNearestResource(info, f, RESOURCE_ELEPHANT);
        return sn;
    }

    void assignIdleWorkers(UsrAI *ai, tagInfo &info)
    {
        int landFarmers = 0;
        for (const tagFarmer &f : info.farmers)
            if (f.FarmerSort == FARMERTYPE_FARMER) ++landFarmers;
        if (landFarmers == 0) return;

        int woodNow = currentWorkersOnType(info, RESOURCE_TREE);
        int stoneNow = currentWorkersOnType(info, RESOURCE_STONE);
        int goldNow = currentWorkersOnType(info, RESOURCE_GOLD);
        int foodNow = currentWorkersOnType(info, RESOURCE_BUSH) +
                      currentWorkersOnType(info, RESOURCE_GAZELLE) +
                      currentWorkersOnType(info, RESOURCE_ELEPHANT) +
                      currentFarmWorkers(info);

        bool bronze = info.civilizationStage >= CIVILIZATION_BRONZEAGE;

        // 铜器前的核心目标是第二波前完成时代升级。
        // 第一波结束后把大多数空闲工人压到食物，木头只维持建房/关键建筑，石头只保留少量。
        bool bronzeRush = (!bronze && info.GameFrame >= UsrAIConfig::BRONZE_RUSH_START_FRAME);
        int targetFood  = bronze ? max(8, landFarmers * 45 / 100)
                                 : (bronzeRush ? max(8, landFarmers * 80 / 100)
                                               : max(6, landFarmers * 55 / 100));
        int targetWood  = bronze ? max(5, landFarmers * 25 / 100)
                                 : (bronzeRush ? max(2, landFarmers * 20 / 100)
                                               : max(3, landFarmers * 30 / 100));
        int targetStone = bronze ? max(2, landFarmers * 10 / 100)
                                 : (bronzeRush ? 0 : max(1, landFarmers * 15 / 100));
        int targetGold  = bronze ? max(3, landFarmers * 20 / 100) : 0;

        for (tagFarmer &f : info.farmers)
        {
            if (f.FarmerSort != FARMERTYPE_FARMER || f.NowState != HUMAN_STATE_IDLE) continue;
            if (!canOrder(f.SN, info.GameFrame, 12)) continue;

            int targetSN = -1;
            if (foodNow < targetFood)
            {
                targetSN = chooseFoodTarget(info, f);
                if (targetSN != -1) ++foodNow;
            }
            if (targetSN == -1 && woodNow < targetWood)
            {
                targetSN = chooseNearestResource(info, f, RESOURCE_TREE);
                if (targetSN != -1) ++woodNow;
            }
            if (targetSN == -1 && bronze && goldNow < targetGold)
            {
                targetSN = chooseNearestResource(info, f, RESOURCE_GOLD);
                if (targetSN != -1) ++goldNow;
            }
            if (targetSN == -1 && stoneNow < targetStone)
            {
                targetSN = chooseNearestResource(info, f, RESOURCE_STONE);
                if (targetSN != -1) ++stoneNow;
            }
            if (targetSN == -1 && bronze)
            {
                targetSN = chooseNearestResource(info, f, RESOURCE_GOLD);
                if (targetSN == -1) targetSN = chooseFoodTarget(info, f);
            }
            if (targetSN == -1) targetSN = chooseNearestResource(info, f, RESOURCE_TREE);
            if (targetSN == -1) targetSN = chooseFoodTarget(info, f);

            if (targetSN != -1)
            {
                ai->HumanAction(f.SN, targetSN);
                markOrdered(f.SN, info.GameFrame);
            }
        }
    }

    // ---------- 建造与升级 ----------
    bool projectFree(const tagBuilding &b)
    {
        return b.Percent >= 100 && b.Project == 0;
    }

    void requestTech(UsrAI *ai, tagInfo &info, int buildingType, int action,
                     int minFood = 0, int minWood = 0, int minStone = 0, int minGold = 0)
    {
        if (gTechRequested.count(action)) return;
        if (info.Meat < minFood || info.Wood < minWood || info.Stone < minStone || info.Gold < minGold) return;

        tagBuilding *b = findBuilding(info, buildingType);
        if (!b || !projectFree(*b) || !canOrder(b->SN, info.GameFrame, 2)) return;

        ai->BuildingAction(b->SN, action);
        markOrdered(b->SN, info.GameFrame);
        gTechRequested.insert(action);
    }

    void developBase(UsrAI *ai, tagInfo &info)
    {
        int pop = populationUsed(info);

        // 满足两个工具时代建筑后，时代升级拥有最高优先级。
        int toolComplete = 0;
        for (const tagBuilding &b : info.buildings)
            if ((b.Type == BUILDING_MARKET || b.Type == BUILDING_STABLE ||
                 b.Type == BUILDING_RANGE) && b.Percent >= 100)
                ++toolComplete;

        if (info.civilizationStage == CIVILIZATION_TOOLAGE &&
            toolComplete >= 2 && info.Meat >= 800)
        {
            tagBuilding *center = findBuilding(info, BUILDING_CENTER);
            if (center && projectFree(*center) && canOrder(center->SN, info.GameFrame, 2) &&
                !gTechRequested.count(BUILDING_CENTER_UPGRADE))
            {
                ai->BuildingAction(center->SN, BUILDING_CENTER_UPGRADE);
                markOrdered(center->SN, info.GameFrame);
                gTechRequested.insert(BUILDING_CENTER_UPGRADE);
                return;
            }
        }

        // 人口快满时优先补房，并尽量把人口上限推到系统上限 50。
        // 不依赖“固定建多少座房屋”，因为不同任务地图可能预置房屋数量不同。
        if (info.Human_MaxNum < 50 && pop + 2 >= info.Human_MaxNum)
        {
            if (tryBuild(ai, info, BUILDING_HOME, 30)) return;
        }

        // 市场必须先有谷仓；马厩必须先有完成的兵营。
        if (countBuilding(info, BUILDING_GRANARY) == 0)
        {
            if (tryBuild(ai, info, BUILDING_GRANARY, 120)) return;
        }
        if (countBuilding(info, BUILDING_ARMYCAMP) == 0)
        {
            if (tryBuild(ai, info, BUILDING_ARMYCAMP, 125)) return;
        }
        const tagBuilding *granary = findBuildingConst(info, BUILDING_GRANARY);
        if (countBuilding(info, BUILDING_MARKET) == 0 && granary && granary->Percent >= 100)
        {
            if (tryBuild(ai, info, BUILDING_MARKET, 150)) return;
        }
        const tagBuilding *campBuilt = findBuildingConst(info, BUILDING_ARMYCAMP);
        if (countBuilding(info, BUILDING_STABLE) == 0 && campBuilt && campBuilt->Percent >= 100)
        {
            if (tryBuild(ai, info, BUILDING_STABLE, 150)) return;
        }
        // 市场+马厩已经满足升铜前置，靶场延后到铜器时代，节省150木头。
        if (info.civilizationStage >= CIVILIZATION_BRONZEAGE &&
            countBuilding(info, BUILDING_RANGE) == 0 && campBuilt && campBuilt->Percent >= 100)
        {
            if (tryBuild(ai, info, BUILDING_RANGE, 150)) return;
        }

        // 早期解锁箭塔，第一波约 6000 帧到来。
        requestTech(ai, info, BUILDING_GRANARY, BUILDING_GRANARY_ARROWTOWER, 50);

        int earlyTowerTarget = (info.civilizationStage >= CIVILIZATION_BRONZEAGE)
                             ? UsrAIConfig::TARGET_TOWERS : 1;
        if (countBuilding(info, BUILDING_ARROWTOWER) < earlyTowerTarget &&
            info.GameFrame > 2200 && info.Stone >= 150)
        {
            if (tryBuild(ai, info, BUILDING_ARROWTOWER, 0, 150)) return;
        }

        // 铜器前不再抢先研究伐木科技：120食物+75木头会明显拖慢800食物升时代。
        // 到铜器时代后再研究。

        if (info.civilizationStage >= CIVILIZATION_BRONZEAGE)
        {
            if (gBronzeFrame < 0) gBronzeFrame = info.GameFrame;

            // 铜器时代核心科技。先补上铜器前为了抢时代而暂缓的伐木科技。
            requestTech(ai, info, BUILDING_MARKET, BUILDING_MARKET_WOOD_UPGRADE, 120, 75);
            requestTech(ai, info, BUILDING_MARKET, BUILDING_MARKET_WHEEL_UPGRADE, 180, 75);
            requestTech(ai, info, BUILDING_ARMYCAMP, BUILDING_ARMYCAMP_UPGRADE_BROADSWORD, 140, 0, 0, 50);
            requestTech(ai, info, BUILDING_RANGE, BUILDING_RANGE_UPGRADE_COMPOSITE_BOW, 180, 100);
            requestTech(ai, info, BUILDING_GRANARY, BUILDING_GRANARY_ARROWTOWE_UPGRADE, 120, 0, 50);

            if (countBuilding(info, BUILDING_COLLAGE) == 0)
            {
                if (tryBuild(ai, info, BUILDING_COLLAGE, 180)) return;
            }

            // 中后期农田，解决野外食物枯竭。
            if (countBuilding(info, BUILDING_FARM) < UsrAIConfig::TARGET_FARMS && info.Wood >= 100)
            {
                if (tryBuild(ai, info, BUILDING_FARM, 75)) return;
            }

            // 房屋逐步补到人口上限 50，保证后期军队人口。
            if (info.Human_MaxNum < 50 && info.Wood >= 80 &&
                info.GameFrame % 200 < UsrAIConfig::THINK_INTERVAL)
            {
                if (tryBuild(ai, info, BUILDING_HOME, 30)) return;
            }
        }
    }

    void produceFarmers(UsrAI *ai, tagInfo &info)
    {
        tagBuilding *center = findBuilding(info, BUILDING_CENTER);
        if (!center || !projectFree(*center) || !canOrder(center->SN, info.GameFrame, 2)) return;

        int farmerCount = 0;
        for (const tagFarmer &f : info.farmers)
            if (f.FarmerSort == FARMERTYPE_FARMER) ++farmerCount;

        int target = (info.civilizationStage >= CIVILIZATION_BRONZEAGE)
                   ? UsrAIConfig::MAX_ECO_FARMERS
                   : UsrAIConfig::PRE_BRONZE_FARMERS;

        // 升铜条件已经基本具备时，为 800 食物让路，不再把食物全花在村民上。
        if (info.civilizationStage == CIVILIZATION_TOOLAGE &&
            info.GameFrame >= UsrAIConfig::BRONZE_RUSH_START_FRAME)
            return;

        if (farmerCount < target && populationUsed(info) < info.Human_MaxNum && info.Meat >= 50)
        {
            ai->BuildingAction(center->SN, BUILDING_CENTER_CREATEFARMER);
            markOrdered(center->SN, info.GameFrame);
        }
    }

    // ---------- 战斗 ----------
    int enemyPriority(const tagArmy &a)
    {
        if (a.Sort == AT_STONE_THROWER) return 1000;
        if (a.Sort == AT_CHARIOT_ARCHER) return 900;
        if (a.Sort == AT_CAVALRY) return 800;
        if (a.Sort == AT_COMPOSITE_BOWMAN) return 750;
        if (a.Sort == AT_HOPLITE) return 700;
        return 500;
    }

    tagArmy* chooseEnemy(tagInfo &info)
    {
        tagArmy *best = nullptr;
        int bestP = -1;
        for (tagArmy &e : info.enemy_armies)
        {
            int p = enemyPriority(e);
            if (!best || p > bestP)
            {
                best = &e;
                bestP = p;
            }
        }
        return best;
    }

    tagArmy* findPriest(tagInfo &info)
    {
        for (tagArmy &a : info.armies)
            if (a.Sort == AT_PRIEST) return &a;
        return nullptr;
    }

    bool walkableBlock(const tagInfo &info, int x, int y)
    {
        if (!info.theMap) return false;
        if (x < 1 || y < 1 || x >= MAP_L - 1 || y >= MAP_U - 1) return false;
        const tagTerrain &t = (*info.theMap)[x][y];
        if (t.type != MAPPATTERN_GRASS || t.height < 0) return false;

        // 避免把祭司送进建筑/资源碰撞格。
        for (const tagBuilding &b : info.buildings)
        {
            int bs = buildingSize(b.Type);
            if (x >= b.BlockDR && x < b.BlockDR + bs &&
                y >= b.BlockUR && y < b.BlockUR + bs) return false;
        }
        for (const tagResource &r : info.resources)
        {
            if (r.BlockDR == x && r.BlockUR == y) return false;
        }
        return true;
    }

    // 在防御核心周围寻找一个“离敌人尽量远、但仍靠近箭塔/市镇中心”的安全格。
    bool findPriestSafePoint(const tagInfo &info, const tagArmy &priest,
                             double &outDR, double &outUR)
    {
        const tagBuilding *tower = nullptr;
        const tagBuilding *center = findBuildingConst(info, BUILDING_CENTER);

        // 有箭塔时优先躲在箭塔附近，因为第一波最适合用箭塔承担伤害。
        double nearestTower = numeric_limits<double>::max();
        for (const tagBuilding &b : info.buildings)
        {
            if (b.Type != BUILDING_ARROWTOWER || b.Percent < 100) continue;
            double d = blockDistance(priest.BlockDR, priest.BlockUR, b.BlockDR, b.BlockUR);
            if (!tower || d < nearestTower)
            {
                tower = &b;
                nearestTower = d;
            }
        }

        const tagBuilding *anchor = tower ? tower : center;
        if (!anchor) return false;

        double bestScore = -1e18;
        int bestX = -1, bestY = -1;

        // 箭塔附近控制在大约 2~5 格，中心附近控制在 3~6 格。
        int rMin = tower ? 2 : 3;
        int rMax = tower ? 5 : 6;
        for (int dx = -rMax; dx <= rMax; ++dx)
        {
            for (int dy = -rMax; dy <= rMax; ++dy)
            {
                int ad = max(abs(dx), abs(dy));
                if (ad < rMin || ad > rMax) continue;
                int x = anchor->BlockDR + dx;
                int y = anchor->BlockUR + dy;
                if (!walkableBlock(info, x, y)) continue;

                double minEnemy = 1000.0;
                for (const tagArmy &e : info.enemy_armies)
                {
                    double d = blockDistance(x, y, e.BlockDR, e.BlockUR);
                    if (d < minEnemy) minEnemy = d;
                }

                // 敌人越远越好；同时不要离防御核心太远。
                double anchorD = blockDistance(x, y, anchor->BlockDR, anchor->BlockUR);
                double score = minEnemy * 100.0 - anchorD * 5.0;
                if (score > bestScore)
                {
                    bestScore = score;
                    bestX = x; bestY = y;
                }
            }
        }

        if (bestX < 0) return false;
        outDR = (bestX + 0.5) * BLOCKSIDELENGTH;
        outUR = (bestY + 0.5) * BLOCKSIDELENGTH;
        return true;
    }

    double nearestEnemyDistanceToPriest(const tagInfo &info, const tagArmy &priest,
                                        const tagArmy **nearestEnemy = nullptr)
    {
        double best = numeric_limits<double>::max();
        const tagArmy *bestEnemy = nullptr;
        for (const tagArmy &e : info.enemy_armies)
        {
            double d = blockDistance(priest.BlockDR, priest.BlockUR, e.BlockDR, e.BlockUR);
            if (d < best)
            {
                best = d;
                bestEnemy = &e;
            }
        }
        if (nearestEnemy) *nearestEnemy = bestEnemy;
        return best;
    }

    // 第一阶段主动把祭司留在基地防御核心附近，防止探路/游荡导致第一波直接被秒。
    void guardPriestEarly(UsrAI *ai, tagInfo &info)
    {
        if (info.GameFrame >= UsrAIConfig::PRIEST_EARLY_GUARD_END) return;
        tagArmy *priest = findPriest(info);
        if (!priest || !canOrder(priest->SN, info.GameFrame, 45)) return;

        const tagBuilding *tower = nullptr;
        for (const tagBuilding &b : info.buildings)
        {
            if (b.Type == BUILDING_ARROWTOWER && b.Percent >= 100)
            {
                tower = &b;
                break;
            }
        }
        const tagBuilding *center = findBuildingConst(info, BUILDING_CENTER);
        const tagBuilding *anchor = tower ? tower : center;
        if (!anchor) return;

        double dHome = blockDistance(priest->BlockDR, priest->BlockUR,
                                     anchor->BlockDR, anchor->BlockUR);
        if (dHome > UsrAIConfig::PRIEST_HOME_RADIUS)
        {
            double dr = 0.0, ur = 0.0;
            if (findPriestSafePoint(info, *priest, dr, ur))
            {
                ai->HumanMove(priest->SN, dr, ur);
                markOrdered(priest->SN, info.GameFrame);
            }
        }
    }

    void defend(UsrAI *ai, tagInfo &info)
    {
        tagArmy *enemy = chooseEnemy(info);
        if (!enemy) return;

        // 所有可用军队集中攻击高威胁目标。
        for (tagArmy &a : info.armies)
        {
            if (a.Sort == AT_PRIEST) continue;
            if (a.NowState == HUMAN_STATE_IDLE && canOrder(a.SN, info.GameFrame, 10))
            {
                ai->HumanAction(a.SN, enemy->SN);
                markOrdered(a.SN, info.GameFrame);
            }
        }

        // 祭司逻辑改成“绝对保命优先”。不能只看当前攻击目标，必须看离祭司最近的敌军。
        tagArmy *priest = findPriest(info);
        if (!priest || !canOrder(priest->SN, info.GameFrame, 12)) return;

        const tagArmy *nearest = nullptr;
        double nearestD = nearestEnemyDistanceToPriest(info, *priest, &nearest);
        int dangerRange = UsrAIConfig::PRIEST_DANGER_RANGE;
        if (nearest && (nearest->Sort == AT_CHARIOT_ARCHER ||
                        nearest->Sort == AT_CAVALRY ||
                        nearest->Sort == AT_STONE_THROWER))
            dangerRange = UsrAIConfig::PRIEST_PANIC_RANGE;

        // 任意敌人进入警戒范围，立即撤到箭塔/市镇中心后方的安全格。
        if (nearest && nearestD < dangerRange)
        {
            double dr = 0.0, ur = 0.0;
            if (findPriestSafePoint(info, *priest, dr, ur))
            {
                ai->HumanMove(priest->SN, dr, ur);
                markOrdered(priest->SN, info.GameFrame);
            }
            return;
        }

        // 第一波阶段完全禁止祭司主动转化。任务文档明确祭司死亡直接失败，
        // 因此宁可少拿一个敌军，也不允许祭司为了转化走到前线。
        if (info.GameFrame < UsrAIConfig::PRIEST_EARLY_GUARD_END) return;

        // 第二阶段以后，只有目标明显安全时才转化。
        if (enemy && priest->ConvertCooldown <= 0)
        {
            double targetD = blockDistance(priest->BlockDR, priest->BlockUR,
                                           enemy->BlockDR, enemy->BlockUR);
            if (targetD >= 8.0 && targetD <= 11.0 && nearestD >= 10.0)
            {
                ai->HumanAction(priest->SN, enemy->SN);
                markOrdered(priest->SN, info.GameFrame);
            }
        }
    }


    // 第一波专用：完全复刻“选中单位/箭塔 -> 右键敌人”的行为。
    // Core.cpp 已确认 HumanAction 对 Farmer、Army、ArrowTower 都会建立 CoreEven_Attacking。
    bool firstWaveAllInAttack(UsrAI *ai, tagInfo &info)
    {
        if (info.enemy_armies.empty()) return false;
        if (info.GameFrame > UsrAIConfig::FIRST_WAVE_END_FRAME) return false;

        // 把敌人按“离基地近 + 远程优先”排序，保证三个目标都有人打。
        vector<tagArmy*> enemies;
        for (tagArmy &e : info.enemy_armies) enemies.push_back(&e);
        const tagBuilding *center = findBuildingConst(info, BUILDING_CENTER);

        sort(enemies.begin(), enemies.end(),
             [center](const tagArmy *a, const tagArmy *b)
             {
                 int pa = enemyPriority(*a);
                 int pb = enemyPriority(*b);
                 if (pa != pb) return pa > pb;
                 if (!center) return a->SN < b->SN;
                 double da = blockDistance(a->BlockDR, a->BlockUR, center->BlockDR, center->BlockUR);
                 double db = blockDistance(b->BlockDR, b->BlockUR, center->BlockDR, center->BlockUR);
                 return da < db;
             });

        const int nEnemy = static_cast<int>(enemies.size());
        if (nEnemy == 0) return false;

        // 1) 箭塔：直接 HumanAction(towerSN, enemySN)，等价于手动选箭塔后右键敌人。
        // 每座塔选择离自己最近的敌人；只在需要切目标时补命令，避免不断重置攻击。
        for (tagBuilding &tower : info.buildings)
        {
            if (tower.Type != BUILDING_ARROWTOWER || tower.Percent < 100) continue;

            tagArmy *target = nullptr;
            double bestD = numeric_limits<double>::max();
            for (tagArmy *e : enemies)
            {
                double d = blockDistance(tower.BlockDR, tower.BlockUR, e->BlockDR, e->BlockUR);
                if (!target || d < bestD)
                {
                    target = e;
                    bestD = d;
                }
            }

            if (target && canOrder(tower.SN, info.GameFrame, 20))
            {
                ai->HumanAction(tower.SN, target->SN);
                markOrdered(tower.SN, info.GameFrame);
            }
        }

        // 2) 所有非祭司士兵：不管当前是空闲、走路还是正在做别的，全都投入战斗。
        int fighterIndex = 0;
        for (tagArmy &a : info.armies)
        {
            if (a.Sort == AT_PRIEST) continue;

            // 多人打一个：轮流分配，敌人越少，每个敌人自然得到越多人围殴。
            tagArmy *target = enemies[fighterIndex % nEnemy];
            ++fighterIndex;

            // 如果已经锁定该目标并正在攻击，就不要重复下令；否则补一次“右键敌人”。
            if (!(a.WorkObjectSN == target->SN && a.NowState == HUMAN_STATE_ATTACKING) &&
                canOrder(a.SN, info.GameFrame, 18))
            {
                ai->HumanAction(a.SN, target->SN);
                markOrdered(a.SN, info.GameFrame);
            }
        }

        // 3) 所有陆地工人暂停经济，直接“右键敌人”参与围殴。
        int workerIndex = 0;
        for (tagFarmer &f : info.farmers)
        {
            if (f.FarmerSort != FARMERTYPE_FARMER) continue;

            // 工人优先补到人数最少的目标；简单轮转即可保证三个敌人都有人贴身。
            tagArmy *target = enemies[workerIndex % nEnemy];
            ++workerIndex;

            if (!(f.WorkObjectSN == target->SN && f.NowState == HUMAN_STATE_ATTACKING) &&
                canOrder(f.SN, info.GameFrame, 18))
            {
                ai->HumanAction(f.SN, target->SN);
                markOrdered(f.SN, info.GameFrame);
            }
        }

        return true;
    }

    void produceArmy(UsrAI *ai, tagInfo &info)
    {
        int used = populationUsed(info);
        if (used >= info.Human_MaxNum) return;

        int armyN = militaryCount(info);
        bool bronze = info.civilizationStage >= CIVILIZATION_BRONZEAGE;

        // 铜器前留一些经济资源，但仍要在第一波前形成最低限度兵力。
        if (!bronze)
        {
            // 冲铜开始后完全停止训练工具时代兵，锁住食物用于800食物升级。
            if (info.GameFrame >= UsrAIConfig::BRONZE_RUSH_START_FRAME)
                return;
            // 至少训练 1 名侦察骑兵。后续自动探索地图、发现黄金和敌方基地都依赖它。
            bool hasScout = false;
            for (const tagArmy &a : info.armies)
                if (a.Sort == AT_SCOUT) { hasScout = true; break; }

            tagBuilding *stable = findBuilding(info, BUILDING_STABLE);
            if (!hasScout && stable && projectFree(*stable) && info.Meat >= 60)
            {
                ai->BuildingAction(stable->SN, BUILDING_STABLE_CREATE_SCOUT);
                markOrdered(stable->SN, info.GameFrame);
                return;
            }

            if (armyN >= 5) return;

            tagBuilding *range = findBuilding(info, BUILDING_RANGE);
            if (range && projectFree(*range) && info.Meat >= 40 && info.Wood >= 20)
            {
                ai->BuildingAction(range->SN, BUILDING_RANGE_CREATE_BOWMAN);
                markOrdered(range->SN, info.GameFrame);
                return;
            }
            tagBuilding *camp = findBuilding(info, BUILDING_ARMYCAMP);
            if (camp && projectFree(*camp) && info.Meat >= 50)
            {
                ai->BuildingAction(camp->SN, BUILDING_ARMYCAMP_CREATE_CLUBMAN);
                markOrdered(camp->SN, info.GameFrame);
            }
            return;
        }

        // 铜器时代：学院方阵兵 > 骑兵 > 复合弓/普通弓 > 阔剑。
        tagBuilding *college = findBuilding(info, BUILDING_COLLAGE);
        if (college && projectFree(*college) && info.Meat >= 60 && info.Gold >= 40)
        {
            ai->BuildingAction(college->SN, BUILDING_COLLAGE_CREATE_HOPLITE);
            markOrdered(college->SN, info.GameFrame);
            return;
        }

        tagBuilding *stable = findBuilding(info, BUILDING_STABLE);
        if (stable && projectFree(*stable) && info.Meat >= 70 && info.Gold >= 80)
        {
            ai->BuildingAction(stable->SN, BUILDING_STABLE_CREATE_CAVALRY);
            markOrdered(stable->SN, info.GameFrame);
            return;
        }

        tagBuilding *range = findBuilding(info, BUILDING_RANGE);
        if (range && projectFree(*range))
        {
            if (gTechRequested.count(BUILDING_RANGE_UPGRADE_COMPOSITE_BOW) && info.Meat >= 40 && info.Gold >= 20)
            {
                ai->BuildingAction(range->SN, BUILDING_RANGE_CREATE_COMPOSITE_BOWMAN);
                markOrdered(range->SN, info.GameFrame);
                return;
            }
            if (info.Meat >= 40 && info.Wood >= 20)
            {
                ai->BuildingAction(range->SN, BUILDING_RANGE_CREATE_BOWMAN);
                markOrdered(range->SN, info.GameFrame);
                return;
            }
        }

        tagBuilding *camp = findBuilding(info, BUILDING_ARMYCAMP);
        if (camp && projectFree(*camp))
        {
            if (gTechRequested.count(BUILDING_ARMYCAMP_UPGRADE_BROADSWORD) && info.Meat >= 35 && info.Gold >= 15)
                ai->BuildingAction(camp->SN, BUILDING_ARMYCAMP_CREATE_BROADSWORD);
            else if (info.Meat >= 50)
                ai->BuildingAction(camp->SN, BUILDING_ARMYCAMP_CREATE_CLUBMAN);
            markOrdered(camp->SN, info.GameFrame);
        }
    }

    // ---------- 探索与总攻 ----------
    void explore(UsrAI *ai, tagInfo &info)
    {
        if (info.civilizationStage < CIVILIZATION_BRONZEAGE) return;
        if (!info.enemy_armies.empty()) return;

        // 优先用侦察骑兵探路，其次其他空闲军队；不拿祭司探危险区域。
        tagArmy *scout = nullptr;
        for (tagArmy &a : info.armies)
        {
            if (a.Sort == AT_SCOUT && a.NowState == HUMAN_STATE_IDLE) { scout = &a; break; }
        }
        if (!scout) return;
        if (!canOrder(scout->SN, info.GameFrame, 120)) return;

        // 覆盖地图四角、四边中点和中心附近。即使某点不可达，下一轮也会换目标。
        static const int points[][2] = {
            {12,12},{12,50},{12,87},{50,87},{87,87},{87,50},{87,12},{50,12},
            {25,25},{25,75},{75,75},{75,25},{50,50}
        };
        const int N = sizeof(points) / sizeof(points[0]);
        int idx = gExploreStep++ % N;
        ai->HumanMove(scout->SN,
                      points[idx][0] * BLOCKSIDELENGTH,
                      points[idx][1] * BLOCKSIDELENGTH);
        markOrdered(scout->SN, info.GameFrame);
    }

    tagBuilding* enemySiege(tagInfo &info)
    {
        for (tagBuilding &b : info.enemy_buildings)
            if (b.Type == BUILDING_SIEGE) return &b;
        return nullptr;
    }

    tagBuilding* dangerousEnemyBuilding(tagInfo &info, const tagBuilding *siege)
    {
        tagBuilding *best = nullptr;
        double bestD = numeric_limits<double>::max();

        // 箭塔最高优先级。若知道攻城武器厂，优先清其周围箭塔。
        for (tagBuilding &b : info.enemy_buildings)
        {
            if (b.Type != BUILDING_ARROWTOWER) continue;
            double d = siege ? blockDistance(b.BlockDR,b.BlockUR,siege->BlockDR,siege->BlockUR) : 0.0;
            if (!best || d < bestD)
            {
                best = &b;
                bestD = d;
            }
        }
        return best;
    }

    void attackEnemyBase(UsrAI *ai, tagInfo &info)
    {
        if (info.GameFrame < UsrAIConfig::ATTACK_START_FRAME) return;

        tagBuilding *siege = enemySiege(info);
        if (!siege)
        {
            explore(ai, info);
            return;
        }

        // 有守军时仍按防守逻辑集中火力，尤其优先投石车和战车弓兵。
        if (!info.enemy_armies.empty())
        {
            defend(ai, info);
            return;
        }

        tagBuilding *tower = dangerousEnemyBuilding(info, siege);
        int fighters = militaryCount(info);

        // 主力先清箭塔；没有箭塔后再让军队压到攻城武器厂附近吸引并清守军。
        if (tower)
        {
            for (tagArmy &a : info.armies)
            {
                if (a.Sort == AT_PRIEST) continue;
                if (a.NowState == HUMAN_STATE_IDLE && canOrder(a.SN, info.GameFrame, 15))
                {
                    ai->HumanAction(a.SN, tower->SN);
                    markOrdered(a.SN, info.GameFrame);
                }
            }
            return;
        }

        if (fighters >= 8)
        {
            for (tagArmy &a : info.armies)
            {
                if (a.Sort == AT_PRIEST) continue;
                if (a.NowState == HUMAN_STATE_IDLE && canOrder(a.SN, info.GameFrame, 20))
                {
                    // 直接攻击攻城武器厂会让部队走到敌方核心，途中会自动遭遇守军。
                    // 目标不是摧毁它，所以只让部分军队压过去，其余仍会在发现敌军后切换目标。
                    ai->HumanMove(a.SN,
                                  (siege->BlockDR - 5) * BLOCKSIDELENGTH,
                                  (siege->BlockUR - 5) * BLOCKSIDELENGTH);
                    markOrdered(a.SN, info.GameFrame);
                }
            }
        }

        // 当视野内已无敌军、附近已无已知箭塔，而且有足够护卫后，祭司执行最终转换。
        tagArmy *priest = findPriest(info);
        if (priest && fighters >= 10 && priest->ConvertCooldown <= 0 &&
            canOrder(priest->SN, info.GameFrame, 40))
        {
            ai->HumanAction(priest->SN, siege->SN);
            markOrdered(priest->SN, info.GameFrame);
        }
    }
}

void UsrAI::processData()
{
    tagInfo info = getInfo();

    // 限频：文档明确说明 processData 每帧调用一次，不能每帧重复发同一批命令。
    if (info.GameFrame - gLastThinkFrame < UsrAIConfig::THINK_INTERVAL)
        return;
    gLastThinkFrame = info.GameFrame;

    // 0) 第一阶段先把祭司固定在基地防御核心附近。
    //    祭司死亡会直接失败，因此其安全优先级高于探路和转化。
    guardPriestEarly(this, info);

    // 1) 第一波：箭塔 + 所有士兵 + 所有陆地工人直接执行“右键敌人”式攻击。
    // 战斗期间工人暂时不采集，保证命令不被经济逻辑覆盖。
    bool firstWaveFighting = firstWaveAllInAttack(this, info);

    // 2) 非第一波敌军仍走常规防守逻辑。
    if (!firstWaveFighting && !info.enemy_armies.empty())
        defend(this, info);

    // 3) 科技/建筑仍继续推进，尤其第一波后立刻抢800食物升铜。
    developBase(this, info);
    produceFarmers(this, info);

    // 第一波正在交战时，绝不让 assignIdleWorkers 把工人的攻击命令覆盖。
    if (!firstWaveFighting)
        assignIdleWorkers(this, info);

    // 4) 持续扩军。第一波新生产的士兵下一轮会自动加入攻击；
    // 第一波后若已有基础兵力，则停止烧食物，优先第二波前进入铜器时代。
    produceArmy(this, info);

    // 4) 铜器后让侦察骑兵持续开图；第三波后进入总攻流程。
    if (info.GameFrame >= UsrAIConfig::ATTACK_START_FRAME)
        attackEnemyBase(this, info);
    else
        explore(this, info);
}

