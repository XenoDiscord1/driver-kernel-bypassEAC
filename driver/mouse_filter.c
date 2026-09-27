// language: C, file: mouse_filter.c, target: Windows 10/11 x64 kernel, WDK
// Upper filter для mouclass: перехватывает ServiceCallback мышиного класс-драйвера.
// Офсеты вставляются как MOUSE_INPUT_DATA напрямую в стек ввода —
// injected-бит не выставляется, ввод неотличим от физической мыши.
#include "driver.h"

// ─── Структуры mouclass ───────────────────────────────────────────────────
typedef VOID(*PMOUSE_SERVICE_CALLBACK)(
    PDEVICE_OBJECT DevObj,
    PMOUSE_INPUT_DATA InputDataStart,
    PMOUSE_INPUT_DATA InputDataEnd,
    PULONG            InputDataConsumed
);

// Сохраняем оригинальный ServiceCallback при подключении
typedef struct _MOUSE_FILTER_CTX {
    PMOUSE_SERVICE_CALLBACK OriginalCallback;
    PDEVICE_OBJECT          ClassDevObj;
    RC_DEV_CTX*             RcCtx;       // ссылка на основной контекст
} MOUSE_FILTER_CTX;

static MOUSE_FILTER_CTX g_filter = { 0 };

// ─── Наш ServiceCallback (вызывается вместо оригинального) ───────────────
// Здесь мы можем:
//   1. Пропустить входящие данные (оригинальный поток мыши)
//   2. Вставить дополнительные MOUSE_INPUT_DATA для компенсации
VOID RcMouseServiceCallback(
    PDEVICE_OBJECT DevObj,
    PMOUSE_INPUT_DATA InputDataStart,
    PMOUSE_INPUT_DATA InputDataEnd,
    PULONG InputDataConsumed)
{
    // Сначала передаём оригинальный ввод
    if (g_filter.OriginalCallback) {
        g_filter.OriginalCallback(DevObj, InputDataStart,
                                  InputDataEnd, InputDataConsumed);
    }

    // Теперь применяем накопленные офсеты из кольцевой очереди
    RC_DEV_CTX* ctx = g_filter.RcCtx;
    if (!ctx || !InterlockedCompareExchange(&ctx->active, 1, 1)) return;

    LONG head = ctx->queue.head & RC_QUEUE_MASK;
    LONG tail = ctx->queue.tail & RC_QUEUE_MASK;

    while (tail != head) {
        RC_OFFSET_MSG* msg = &ctx->queue.buf[tail];

        // Создаём синтетический MOUSE_INPUT_DATA
        MOUSE_INPUT_DATA synthetic = { 0 };
        synthetic.UnitId    = 0;        // тот же юнит что и реальная мышь
        synthetic.Flags     = MOUSE_MOVE_RELATIVE;
        synthetic.LastX     = (LONG)msg->x;
        synthetic.LastY     = (LONG)msg->y;

        ULONG consumed = 0;

        // Инжектируем через оригинальный callback как ещё один пакет ввода.
        // С точки зрения системы это физическое движение мышью.
        if (g_filter.OriginalCallback) {
            g_filter.OriginalCallback(
                DevObj,
                &synthetic,
                &synthetic + 1,
                &consumed
            );
        }

        tail = (tail + 1) & RC_QUEUE_MASK;
        InterlockedExchange(&ctx->queue.tail, tail);
        InterlockedIncrement((PLONG)&ctx->applied_total);
    }
}

// ─── Установка фильтра через IOCTL_INTERNAL_MOUSE_CONNECT ────────────────
// Вызывается при DeviceAdd фильтрующего устройства.
// Мы перехватываем CONNECT_DATA чтобы подменить ServiceCallback.
NTSTATUS RcInstallMouseFilter(
    PDEVICE_OBJECT FilterDO,
    PDEVICE_OBJECT LowerDO,
    RC_DEV_CTX*    RcCtx)
{
    // CONNECT_DATA описывает обратный вызов mouclass
    typedef struct _CONNECT_DATA {
        PDEVICE_OBJECT ClassDeviceObject;
        PVOID          ClassService;
    } CONNECT_DATA, *PCONNECT_DATA;

    CONNECT_DATA connect;
    connect.ClassDeviceObject = FilterDO;
    connect.ClassService      = RcMouseServiceCallback;

    KEVENT           evt;
    IO_STATUS_BLOCK  iosb;
    KeInitializeEvent(&evt, NotificationEvent, FALSE);

    PIRP irp = IoBuildDeviceIoControlRequest(
        IOCTL_INTERNAL_MOUSE_CONNECT,
        LowerDO,
        &connect, sizeof(connect),
        nullptr, 0,
        TRUE, &evt, &iosb
    );
    if (!irp) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS st = IoCallDriver(LowerDO, irp);
    if (st == STATUS_PENDING)
        KeWaitForSingleObject(&evt, Executive, KernelMode, FALSE, nullptr);

    st = iosb.Status;

    if (NT_SUCCESS(st)) {
        // Mouclass вернул оригинальный ClassService через connect.ClassService
        g_filter.OriginalCallback = (PMOUSE_SERVICE_CALLBACK)connect.ClassService;
        g_filter.ClassDevObj      = connect.ClassDeviceObject;
        g_filter.RcCtx            = RcCtx;
        // ctx->lower_mouse нужен для future IRP forwarding
        RcCtx->lower_mouse = LowerDO;
    }

    return st;
}

// ─── Получить нижнее устройство mouclass ──────────────────────────────────
// Проходим по стеку драйверов от \Driver\mouclass PDO
PDEVICE_OBJECT RcFindMouclassDevice(VOID) {
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Driver\\mouclass");
    PDRIVER_OBJECT drv  = nullptr;

    NTSTATUS st = ObReferenceObjectByName(
        &name,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        nullptr,
        0,
        *IoDriverObjectType,
        KernelMode,
        nullptr,
        (PVOID*)&drv
    );
    if (!NT_SUCCESS(st) || !drv) return nullptr;

    PDEVICE_OBJECT dev = drv->DeviceObject;
    if (dev) ObReferenceObject(dev);
    ObDereferenceObject(drv);
    return dev;
}
