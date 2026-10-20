// Identical eligible programmes share one draw weight; their IDs stay intact.
#ifndef TRACKPROGRAMLOTTERY_H
#define TRACKPROGRAMLOTTERY_H
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QtGlobal>

struct TrackProgrammeTicket
{
    quint32 id;
    QByteArray key;
    int weight;
    qint64 last;
};

inline quint32 trackProgrammeDraw(const QList<TrackProgrammeTicket> &tickets,
                                 int cursor, qint64 since, qint64 cooldown, quint32 invalid)
{
    struct Family { QList<TrackProgrammeTicket> aliases; int weight = 0; qint64 last = -1; };
    QList<Family> families;
    QHash<QByteArray,int> index;
    for (const TrackProgrammeTicket &ticket : tickets)
    {
        const QByteArray key = ticket.key.isEmpty() ? QByteArray("id:") + QByteArray::number(ticket.id) : ticket.key;
        if (!index.contains(key))
        {
            index.insert(key, int(families.size()));
            families.append(Family());
        }
        Family &family = families[index.value(key)];
        family.aliases.append(ticket);
        family.weight = qMax(family.weight, ticket.weight);
        family.last = qMax(family.last, ticket.last);
    }
    QList<int> rested;
    for (int i=0; i<int(families.size()); ++i)
    {
        const qint64 last = families.at(i).last;
        if (last < 0 || last > since || since - last >= cooldown)
            rested.append(i);
    }
    // The fallback counts distinct outputs, never the number of aliases.
    if (rested.size() < 2)
    {
        rested.clear();
        for (int i=0; i<int(families.size()); ++i) rested.append(i);
    }
    int top = 0;
    for (int i : rested) top = qMax(top, families.at(i).weight);
    QList<int> pool;
    for (int k=1; k<=top; ++k)
        for (int i : rested)
            if (families.at(i).weight >= k) pool.append(i);
    if (pool.isEmpty()) return invalid;
    const quint64 pos = cursor < 0 ? quint64(-qint64(cursor)) : quint64(cursor);
    const Family &family = families.at(pool.at(pos % quint64(pool.size())));
    // Ratings remain attached to the real function, including which alias
    // is played. Max (rather than sum) prevents extra copies adding weight.
    QList<quint32> aliases;
    for (int k=1; k<=family.weight; ++k)
        for (const TrackProgrammeTicket &ticket : family.aliases)
            if (ticket.weight >= k) aliases.append(ticket.id);
    return aliases.at((pos / quint64(pool.size())) % quint64(aliases.size()));
}
#endif
