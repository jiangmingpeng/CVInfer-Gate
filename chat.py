from openai import OpenAI

client = OpenAI(base_url="http://localhost:8000/v1", api_key="dummy")

print("输入内容开始聊天，输入 exit 退出。")
history = []

while True:
    user = input("\n你: ")
    if user.strip().lower() in ("exit", "quit"):
        break
    history.append({"role": "user", "content": user})

    resp = client.chat.completions.create(
        model="Qwen/Qwen2-VL-2B-Instruct",
        messages=history,
        max_tokens=512,
        temperature=0.7,
    )
    answer = resp.choices[0].message.content
    print(f"\nAI: {answer}")
    history.append({"role": "assistant", "content": answer})
