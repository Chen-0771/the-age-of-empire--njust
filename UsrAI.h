#ifndef USRAI_H
#define USRAI_H

#include "ai.h"
#include <unordered_map>

extern tagGame tagUsrGame;
extern ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

class UsrAI:public AI
{
public:
    UsrAI(){this->id=0;}
    ~UsrAI(){}

private:
    void processData() override;
    tagInfo getInfo(){return tagUsrGame.getInfo();}
    int AddToIns(instruction ins) override
    {
        UsrIns.lock.lock();
        ins.id=UsrIns.g_id;
        UsrIns.g_id++;
        UsrIns.instructions.push(ins);
        UsrIns.lock.unlock();
        return ins.id;
    }
    void clearInsRet() override
    {
        tagUsrGame.clearInsRet();
    }
    /*##########DO NOT MODIFY THE CODE IN THE CLASS##########*/
    void collectResource();
    void arrowTowerAttack(tagBuilding);
    void getLegalPlace(tagObj, int, int);


};

/*##########YOUR CODE BEGINS HERE##########*/

// 下面的常量只用于调节 AI 策略，不修改游戏本身的任何规则。
// 如果第一次实机测试发现发展速度偏慢，可以优先调这几个数。
namespace UsrAIConfig
{
    const int THINK_INTERVAL = 8;          // 每隔多少帧做一次完整决策，避免重复下命令
    const int BUILD_RETRY_INTERVAL = 35;   // 建筑失败后换地点重试的间隔
    const int PRE_BRONZE_FARMERS = 14;     // 第一波后继续造人，接近800食物时再停产冲铜
    const int MAX_ECO_FARMERS = 22;        // 升铜后经济人口上限
    const int TARGET_HOUSES = 10;          // 10 房 + TC => 足够接近 50 人口上限
    const int TARGET_FARMS = 9;            // 中后期农田数量
    const int TARGET_TOWERS = 2;           // 第一波前尽量完成2座箭塔，互相掩护并承担仇恨
    const int EARLY_TOWER_FRAME = 700;     // 谷仓科技完成后立即尝试造塔，不再等到2200帧
    const int ATTACK_START_FRAME = 23500;  // 第三波约 21000 帧，稍后开始反攻
    const int BRONZE_RUSH_START_FRAME = 3200; // 第一波前就开始攒800食物，争取第二波前完成升级
    const int SECOND_WAVE_FRAME = 13500;      // 第二波约在13500帧出现
    const int FIRST_WAVE_END_FRAME = 12500;   // 持续清完第一波，避免残余敌人在逻辑切换后存活
    const int PRE_BRONZE_ARMY_TARGET = 10;     // 第一波结束后先补足基础兵力，再攒食物升铜
    const int SECOND_WAVE_ARMY_TARGET = 14;    // 第二波交战时持续补充多兵种防线

    // 祭司保命参数：第一阶段宁可少转化，也不能让祭司冒险。
    const int PRIEST_EARLY_GUARD_END = 8000; // 第一波结束前后，祭司只保命不主动转化
    const int PRIEST_DANGER_RANGE = 12;      // 任意敌军进入此范围，立刻撤退
    const int PRIEST_PANIC_RANGE = 16;       // 战车弓/骑兵等高威胁单位使用更大的警戒半径
    const int PRIEST_HOME_RADIUS = 5;        // 无敌军时也不让祭司离基地防御核心太远
}

/*##########YOUR CODE ENDS HERE##########*/
#endif // USRAI_H
