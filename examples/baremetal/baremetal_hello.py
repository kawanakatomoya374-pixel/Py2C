import asyncio

def pulses():
    yield 7
    yield 9

async def sensor_read():
    return 17

async def boot_task():
    value = await sensor_read()
    return value + 5

for pulse in pulses():
    print(pulse)

print(asyncio.run(boot_task()))
