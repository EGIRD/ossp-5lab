import pandas as pd
import matplotlib.pyplot as plt

df1 = pd.read_csv('data/exp_1client.csv')
df3 = pd.read_csv('data/exp_3clients.csv')

# График 1: Сравнение среднего RTT
labels = ['1 клиент\n(small.txt, 5 строк)', '3 клиента\n(large.txt, 50 строк)']
avg_rtt = [df1['avg_rtt_ms'].mean(), df3['avg_rtt_ms'].mean()]

plt.figure(figsize=(8, 5))
bars = plt.bar(labels, avg_rtt, color=['#2b5c8f', '#d95f02'], width=0.45)
plt.title('Зависимость среднего времени отклика (RTT) от нагрузки и размера данных')
plt.ylabel('Средний RTT, мс')
plt.grid(axis='y', linestyle='--', alpha=0.6)
for bar in bars:
    yval = bar.get_height()
    plt.text(bar.get_x() + bar.get_width()/2, yval + 0.005, f'{yval:.3f} мс', ha='center', va='bottom')
plt.tight_layout()
plt.savefig('diagrams/graph_rtt.png', dpi=300)

# График 2: Суммарная пропускная способность
plt.figure(figsize=(8, 5))
runs = [1, 2, 3]
tp_1 = df1['throughput_rps'].values[:3]
tp_3 = df3.groupby(df3.index // 3)['throughput_rps'].sum().values[:3]

plt.plot(runs, tp_1, marker='o', label='1 активный клиент (small.txt)')
plt.plot(runs, tp_3, marker='s', label='3 активных клиента суммарно (large.txt)')
plt.title('Пропускная способность сервера по сериям запусков')
plt.xlabel('Номер запуска')
plt.ylabel('Пропускная способность, запросов/с')
plt.xticks(runs)
plt.legend()
plt.grid(True, linestyle='--', alpha=0.6)
plt.tight_layout()
plt.savefig('diagrams/graph_throughput.png', dpi=300)
print("Графики сохранены в diagrams/")