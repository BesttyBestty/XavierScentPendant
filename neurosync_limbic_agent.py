import datetime
import time

class LimbicAgent:
    def __init__(self, name):
        self.name = name
        # 这是我们的"药房" - 只有简单的几种配方，减少决策瘫痪
        self.scent_library = {
            "ENERGY": "🍋 柠檬 + 薄荷 (Lemon Mint) - 提神",
            "CALM": "🌲 雪松 + 薰衣草 (Cedar Lavender) - 镇静",
            "FOCUS": "🌿 迷迭香 (Rosemary) - 专注",
            "COMFORT": "🔥 檀香 (Sandalwood) - 温暖/安抚"
        }

    def get_context(self):
        """
        获取当前的时间上下文
        """
        now = datetime.datetime.now()
        hour = now.hour
        
        if 5 <= hour < 12:
            return "早晨 (Morning)"
        elif 12 <= hour < 18:
            return "下午 (Afternoon)"
        else:
            return "夜晚 (Night)"

    def think(self, user_input):
        """
        这是大脑的核心。
        虽然现在我们用简单的逻辑模拟，
        未来这里只有一行代码：return ask_ai_api(user_input)
        """
        context = self.get_context()
        print(f"\n[{self.name}] 正在分析环境... 时间: {context}")
        print(f"[{self.name}] 正在分析你的情绪: '{user_input}'...")
        
        # --- 模拟 AI 的思考过程 ---
        # 关键词匹配逻辑 (简单粗暴，但有效)
        user_input = user_input.lower()
        
        decision = ""
        reason = ""

        if any(word in user_input for word in ["累", "困", "tired", "sleepy"]):
            if "夜晚" in context:
                decision = "CALM"
                reason = "检测到疲劳且是夜晚，你需要高质量睡眠。"
            else:
                decision = "ENERGY"
                reason = "检测到疲劳但是白天，你需要一点动力。"
                
        elif any(word in user_input for word in ["烦", "焦虑", "anxious", "stress"]):
            decision = "CALM"
            reason = "检测到高压力反应，立即介入镇静。"
            
        elif any(word in user_input for word in ["工作", "学习", "work", "study"]):
            decision = "FOCUS"
            reason = "进入心流模式。"
            
        else:
            decision = "COMFORT"
            reason = "未检测到极端情绪，提供基础情感支持。"

        return decision, reason

    def act(self, scent_code):
        """
        执行层：这里未来会发送信号给 C++ / Arduino
        """
        scent_name = self.scent_library.get(scent_code, "未知配方")
        print("\n" + "="*40)
        print(f"🚀 指令已发送至硬件接口")
        print(f"💨 正在释放: {scent_name}")
        print("="*40 + "\n")

# --- 主程序 ---
if __name__ == "__main__":
    # 初始化你的 Agent
    my_agent = LimbicAgent(name="Limbic_V1")

    print(f"你好，我是 {my_agent.name}。你的情绪调节助手已上线。")
    print("我们要去征服世界了，但在那之前，告诉我你现在的感觉。")
    
    while True:
        try:
            # 1. 获取输入
            user_feeling = input("\n请输入你当下的感觉 (输入 'exit' 退出): ")
            if user_feeling.lower() == 'exit':
                break
                
            # 2. 大脑思考
            decision_code, logic = my_agent.think(user_feeling)
            
            # 3. 反馈思考逻辑
            print(f"💡 分析逻辑: {logic}")
            
            # 4. 执行动作 (模拟喷气)
            my_agent.act(decision_code)
            
            # 模拟硬件冷却时间
            time.sleep(1) 
            
        except KeyboardInterrupt:
            break

    print("系统下线。")